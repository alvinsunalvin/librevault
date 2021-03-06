/* Copyright (C) 2016 Alexander Shishenko <alex@shishenko.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
#include "FolderGroup.h"

#include "IgnoreList.h"
#include "PathNormalizer.h"
#include "control/StateCollector.h"
#include "folder/chunk/ChunkStorage.h"
#include "folder/meta/MetaStorage.h"
#include "folder/transfer/MetaDownloader.h"
#include "folder/transfer/MetaUploader.h"
#include "folder/transfer/Uploader.h"
#include "folder/transfer/Downloader.h"
#include "p2p/P2PFolder.h"
#include <QDir>
#include <QJsonArray>
#ifdef Q_OS_WIN
#   include <windows.h>
#endif

namespace librevault {

FolderGroup::FolderGroup(FolderParams params, StateCollector* state_collector, QObject* parent) :
		QObject(parent),
		params_(std::move(params)),
		state_collector_(state_collector) {
	LOGFUNC();

	/* Creating directories */
	QDir().mkpath(params_.path);
	QDir().mkpath(params_.system_path);
#ifdef Q_OS_WIN
	SetFileAttributesW(params_.system_path.toStdWString().c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif

	LOGD("New folder:"
		<< "Key type=" << (char)params_.secret.get_type()
		<< "Path=" << params_.path
		<< "System path=" << params_.system_path);

	state_collector_->folder_state_set(conv_bytearray(params_.secret.get_Hash()), "secret", QString::fromStdString(params_.secret.string()));

	/* Initializing components */
	path_normalizer_ = std::make_unique<PathNormalizer>(params_);
	ignore_list = std::make_unique<IgnoreList>(params_, *path_normalizer_);

	meta_storage_ = new MetaStorage(params_, ignore_list.get(), path_normalizer_.get(), state_collector_, this);
	chunk_storage_ = new ChunkStorage(params_, meta_storage_, path_normalizer_.get(), this);

	uploader_ = new Uploader(chunk_storage_, this);
	downloader_ = new Downloader(params_, meta_storage_, this);
	meta_uploader_ = new MetaUploader(meta_storage_, chunk_storage_, this);
	meta_downloader_ = new MetaDownloader(meta_storage_, downloader_, this);

	state_pusher_ = new QTimer(this);

	// Connecting signals and slots
	connect(meta_storage_, &MetaStorage::metaAdded, this, &FolderGroup::handle_indexed_meta);
	connect(chunk_storage_, &ChunkStorage::chunkAdded, this, [this](const blob& ct_hash){
		downloader_->notifyLocalChunk(ct_hash);
		uploader_->broadcast_chunk(remotes(), ct_hash);
	});
	connect(downloader_, &Downloader::chunkDownloaded, chunk_storage_, &ChunkStorage::put_chunk);
	connect(state_pusher_, &QTimer::timeout, this, &FolderGroup::push_state);

	// Set up state pusher
	state_pusher_->setInterval(1000);
	state_pusher_->start();

	// Go through index
	QTimer::singleShot(0, this, [=]{
		for(auto& smeta : meta_storage_->getMeta())
			handle_indexed_meta(smeta);
	});
}

FolderGroup::~FolderGroup() {
	state_pusher_->stop();

	state_collector_->folder_state_purge(conv_bytearray(params_.secret.get_Hash()));
	LOGFUNC();
}

/* Actions */
void FolderGroup::handle_indexed_meta(const SignedMeta& smeta) {
	Meta::PathRevision revision = smeta.meta().path_revision();
	bitfield_type bitfield = chunk_storage_->make_bitfield(smeta.meta());

	downloader_->notifyLocalMeta(smeta, bitfield);
	meta_uploader_->broadcast_meta(remotes(), revision, bitfield);
}

// RemoteFolder actions
void FolderGroup::handle_handshake(RemoteFolder* origin) {
	remotes_ready_.insert(origin);
	downloader_->trackRemote(origin);

	connect(origin, &RemoteFolder::rcvdChoke, downloader_, [=]{downloader_->handleChoke(origin);});
	connect(origin, &RemoteFolder::rcvdUnchoke, downloader_, [=]{downloader_->handleUnchoke(origin);});
	connect(origin, &RemoteFolder::rcvdInterested, downloader_, [=]{uploader_->handle_interested(origin);});
	connect(origin, &RemoteFolder::rcvdNotInterested, downloader_, [=]{uploader_->handle_not_interested(origin);});

	connect(origin, &RemoteFolder::rcvdHaveMeta, meta_downloader_, [=](Meta::PathRevision revision, bitfield_type bitfield){
		meta_downloader_->handle_have_meta(origin, revision, bitfield);
	});
	connect(origin, &RemoteFolder::rcvdHaveChunk, downloader_, [=](const blob& ct_hash){
		downloader_->notifyRemoteChunk(origin, ct_hash);
	});
	connect(origin, &RemoteFolder::rcvdMetaRequest, meta_uploader_, [=](Meta::PathRevision path_revision){
		meta_uploader_->handle_meta_request(origin, path_revision);
	});
	connect(origin, &RemoteFolder::rcvdMetaReply, meta_downloader_, [=](const SignedMeta& smeta, const bitfield_type& bitfield){
		meta_downloader_->handle_meta_reply(origin, smeta, bitfield);
	});
	connect(origin, &RemoteFolder::rcvdBlockRequest, uploader_, [=](const blob& ct_hash, uint32_t offset, uint32_t size){
		uploader_->handle_block_request(origin, ct_hash, offset, size);
	});
	connect(origin, &RemoteFolder::rcvdBlockReply, downloader_, [=](const blob& ct_hash, uint32_t offset, const blob& block){
		downloader_->putBlock(ct_hash, offset, block, origin);
	});

	QTimer::singleShot(0, meta_uploader_, [=]{meta_uploader_->handle_handshake(origin);});
}

bool FolderGroup::attach(P2PFolder* remote) {
	if(remotes_.contains(remote)
		|| p2p_folders_digests_.contains(remote->digest())
		|| p2p_folders_endpoints_.contains(remote->endpoint()) ) {
		return false;
	}

	remotes_.insert(remote);
	p2p_folders_endpoints_.insert(remote->endpoint());
	p2p_folders_digests_.insert(remote->digest());

	LOGD("Attached remote " << remote->displayName());

	connect(remote, &RemoteFolder::handshakeSuccess, this, [=]{handle_handshake(remote);});

	emit attached(remote);

	return true;
}

void FolderGroup::detach(P2PFolder* remote) {
	if(! remotes_.contains(remote))
		return;

	emit detached(remote);
	downloader_->untrackRemote(remote);

	p2p_folders_digests_.remove(remote->digest());
	p2p_folders_endpoints_.remove(remote->endpoint());

	remotes_.remove(remote);
	remotes_ready_.remove(remote);

	LOGD("Detached remote " << remote->displayName());
}

QList<RemoteFolder*> FolderGroup::remotes() const {
	return remotes_.toList();
}

QString FolderGroup::log_tag() const {
	return !params_.path.isEmpty() ? params_.path : params_.system_path;
}

void FolderGroup::push_state() {
	// peers
	QJsonArray peers_array;
	for(auto& p2p_folder : remotes_) {
		peers_array.append(p2p_folder->collect_state());
	}
	state_collector_->folder_state_set(folderid(), "peers", peers_array);
	// bandwidth
	state_collector_->folder_state_set(folderid(), "traffic_stats", bandwidth_counter_.heartbeat_json());
}

} /* namespace librevault */
