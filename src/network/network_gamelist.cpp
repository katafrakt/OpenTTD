/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file network_gamelist.cpp This file handles the GameList
 * Also, it handles the request to a server for data about the server
 */

#include "../stdafx.h"
#include "../debug.h"
#include "../window_func.h"
#include "network_internal.h"
#include "network_udp.h"
#include "network_gamelist.h"
#include <atomic>

#include "../safeguards.h"

NetworkGameList *_network_game_list = nullptr;

/** The games to insert when the GUI thread has time for us. */
static std::atomic<NetworkGameList *> _network_game_delayed_insertion_list(nullptr);

/**
 * Add a new item to the linked gamelist, but do it delayed in the next tick
 * or so to prevent race conditions.
 * @param item the item to add. Will be freed once added.
 */
void NetworkGameListAddItemDelayed(NetworkGameList *item)
{
	item->next = _network_game_delayed_insertion_list.load(std::memory_order_relaxed);
	while (!_network_game_delayed_insertion_list.compare_exchange_weak(item->next, item, std::memory_order_acq_rel)) {}
}

/** Perform the delayed (thread safe) insertion into the game list */
static void NetworkGameListHandleDelayedInsert()
{
	while (true) {
		NetworkGameList *ins_item = _network_game_delayed_insertion_list.load(std::memory_order_relaxed);
		while (ins_item != nullptr && !_network_game_delayed_insertion_list.compare_exchange_weak(ins_item, ins_item->next, std::memory_order_acq_rel)) {}
		if (ins_item == nullptr) break; // No item left.

		NetworkGameList *item = NetworkGameListAddItem(ins_item->connection_string);

		if (item != nullptr) {
			if (item->info.server_name.empty()) {
				ClearGRFConfigList(&item->info.grfconfig);
				item->info = {};
				item->info.server_name = ins_item->info.server_name;
				item->online = false;
			}
			item->manually |= ins_item->manually;
			if (item->manually) NetworkRebuildHostList();
			UpdateNetworkGameWindow();
		}
		delete ins_item;
	}
}

/**
 * Add a new item to the linked gamelist. If the IP and Port match
 * return the existing item instead of adding it again
 * @param address the address of the to-be added item
 * @return a point to the newly added or already existing item
 */
NetworkGameList *NetworkGameListAddItem(const std::string &connection_string)
{
	NetworkGameList *item, *prev_item;

	/* Parse the connection string to ensure the default port is there. */
	const std::string resolved_connection_string = ParseConnectionString(connection_string, NETWORK_DEFAULT_PORT).GetAddressAsString(false);

	prev_item = nullptr;
	for (item = _network_game_list; item != nullptr; item = item->next) {
		if (item->connection_string == resolved_connection_string) return item;
		prev_item = item;
	}

	item = new NetworkGameList(resolved_connection_string);

	if (prev_item == nullptr) {
		_network_game_list = item;
	} else {
		prev_item->next = item;
	}

	UpdateNetworkGameWindow();

	return item;
}

/**
 * Remove an item from the gamelist linked list
 * @param remove pointer to the item to be removed
 */
void NetworkGameListRemoveItem(NetworkGameList *remove)
{
	NetworkGameList *prev_item = nullptr;
	for (NetworkGameList *item = _network_game_list; item != nullptr; item = item->next) {
		if (remove == item) {
			if (prev_item == nullptr) {
				_network_game_list = remove->next;
			} else {
				prev_item->next = remove->next;
			}

			/* Remove GRFConfig information */
			ClearGRFConfigList(&remove->info.grfconfig);
			delete remove;

			DEBUG(net, 4, "[gamelist] removed server from list");
			NetworkRebuildHostList();
			UpdateNetworkGameWindow();
			return;
		}
		prev_item = item;
	}
}

static const uint MAX_GAME_LIST_REQUERY_COUNT  = 10; ///< How often do we requery in number of times per server?
static const uint REQUERY_EVERY_X_GAMELOOPS    = 60; ///< How often do we requery in time?
static const uint REFRESH_GAMEINFO_X_REQUERIES = 50; ///< Refresh the game info itself after REFRESH_GAMEINFO_X_REQUERIES * REQUERY_EVERY_X_GAMELOOPS game loops

/** Requeries the (game) servers we have not gotten a reply from */
void NetworkGameListRequery()
{
	NetworkGameListHandleDelayedInsert();

	static uint8 requery_cnt = 0;

	if (++requery_cnt < REQUERY_EVERY_X_GAMELOOPS) return;
	requery_cnt = 0;

	for (NetworkGameList *item = _network_game_list; item != nullptr; item = item->next) {
		item->retries++;
		if (item->retries < REFRESH_GAMEINFO_X_REQUERIES && (item->online || item->retries >= MAX_GAME_LIST_REQUERY_COUNT)) continue;

		/* item gets mostly zeroed by NetworkUDPQueryServer */
		uint8 retries = item->retries;
		NetworkUDPQueryServer(item->connection_string);
		item->retries = (retries >= REFRESH_GAMEINFO_X_REQUERIES) ? 0 : retries;
	}
}

/**
 * Rebuild the GRFConfig's of the servers in the game list as we did
 * a rescan and might have found new NewGRFs.
 */
void NetworkAfterNewGRFScan()
{
	for (NetworkGameList *item = _network_game_list; item != nullptr; item = item->next) {
		/* Reset compatibility state */
		item->info.compatible = item->info.version_compatible;

		for (GRFConfig *c = item->info.grfconfig; c != nullptr; c = c->next) {
			assert(HasBit(c->flags, GCF_COPY));

			const GRFConfig *f = FindGRFConfig(c->ident.grfid, FGCM_EXACT, c->ident.md5sum);
			if (f == nullptr) {
				/* Don't know the GRF, so mark game incompatible and the (possibly)
				 * already resolved name for this GRF (another server has sent the
				 * name of the GRF already. */
				c->name = FindUnknownGRFName(c->ident.grfid, c->ident.md5sum, true);
				c->status = GCS_NOT_FOUND;

				/* If we miss a file, we're obviously incompatible. */
				item->info.compatible = false;
			} else {
				c->filename = f->filename;
				c->name = f->name;
				c->info = f->info;
				c->status = GCS_UNKNOWN;
			}
		}
	}

	InvalidateWindowClassesData(WC_NETWORK_WINDOW);
}
