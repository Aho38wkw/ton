/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "full-node-private-overlay-v2.hpp"

#include "checksum.h"
#include "ton/ton-tl.hpp"
#include "common/delay.h"
#include "td/utils/JsonBuilder.h"
#include "tl/tl_json.h"
#include "auto/tl/ton_api_json.h"
#include "full-node-serializer.hpp"

namespace ton::validator::fullnode {

void FullNodePrivateOverlayV2::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  process_block_broadcast(src, query);
}

void FullNodePrivateOverlayV2::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcastCompressed &query) {
  process_block_broadcast(src, query);
}

void FullNodePrivateOverlayV2::process_block_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  auto B = deserialize_block_broadcast(query, overlay::Overlays::max_fec_broadcast_size());
  if (B.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received block broadcast in private overlay from " << src << ": "
                        << B.ok().block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_broadcast, B.move_as_ok());
}

void FullNodePrivateOverlayV2::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  BlockIdExt block_id = create_block_id(query.block_->block_);
  VLOG(FULL_NODE_DEBUG) << "Received newShardBlockBroadcast in private overlay from " << src << ": "
                        << block_id.to_str();
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block, block_id,
                          query.block_->cc_seqno_, std::move(query.block_->data_));
}

void FullNodePrivateOverlayV2::process_broadcast(PublicKeyHash src,
                                                 ton_api::tonNode_newBlockCandidateBroadcast &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodePrivateOverlayV2::process_broadcast(PublicKeyHash src,
                                                 ton_api::tonNode_newBlockCandidateBroadcastCompressed &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodePrivateOverlayV2::process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  BlockIdExt block_id;
  CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  td::BufferSlice data;
  auto S = deserialize_block_candidate_broadcast(query, block_id, cc_seqno, validator_set_hash, data,
                                                 overlay::Overlays::max_fec_broadcast_size());
  if (S.is_error()) {
    LOG(DEBUG) << "dropped broadcast: " << S;
    return;
  }
  if (data.size() > FullNode::max_block_size()) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with too big size from " << src;
    return;
  }
  if (td::sha256_bits256(data.as_slice()) != block_id.file_hash) {
    VLOG(FULL_NODE_WARNING) << "received block candidate with incorrect file hash from " << src;
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Received newBlockCandidate in private overlay from " << src << ": " << block_id.to_str();
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data));
}

void FullNodePrivateOverlayV2::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }

  ton_api::downcast_call(*B.move_as_ok(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodePrivateOverlayV2::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                     td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newShardBlockBroadcast in private overlay: " << block_id.to_str();
  auto B = create_serialize_tl_object<ton_api::tonNode_newShardBlockBroadcast>(
      create_tl_object<ton_api::tonNode_newShardBlock>(create_tl_block_id(block_id), cc_seqno, std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), 0, std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                            local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
  }
}

void FullNodePrivateOverlayV2::send_broadcast(BlockBroadcast broadcast) {
  if (!inited_) {
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending block broadcast in private overlay (with compression): "
                        << broadcast.block_id.to_str();
  auto B = serialize_block_broadcast(broadcast, true);  // compression_enabled = true
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block broadcast: " << B.move_as_error();
    return;
  }
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodePrivateOverlayV2::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                    td::uint32 validator_set_hash, td::BufferSlice data) {
  if (!inited_) {
    return;
  }
  auto B =
      serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true);  // compression enabled
  if (B.is_error()) {
    VLOG(FULL_NODE_WARNING) << "failed to serialize block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(FULL_NODE_DEBUG) << "Sending newBlockCandidate in private overlay (with compression): " << block_id.to_str();
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_,
                          local_id_.pubkey_hash(), overlay::Overlays::BroadcastFlagAnySender(), B.move_as_ok());
}

void FullNodePrivateOverlayV2::start_up() {
  std::sort(nodes_.begin(), nodes_.end());
  nodes_.erase(std::unique(nodes_.begin(), nodes_.end()), nodes_.end());

  std::vector<td::Bits256> nodes, senders;
  for (const adnl::AdnlNodeIdShort &id : nodes_) {
    nodes.push_back(id.bits256_value());
  }
  for (const adnl::AdnlNodeIdShort &id : senders_) {
    senders.push_back(id.bits256_value());
  }
  auto X = create_hash_tl_object<ton_api::tonNode_privateBlockOverlayIdV2>(
      zero_state_file_hash_, shard_.workchain, shard_.shard, std::move(nodes), std::move(senders));
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();

  try_init();
}

void FullNodePrivateOverlayV2::try_init() {
  // Sometimes adnl id is added to validator engine later (or not at all)
  td::actor::send_closure(
      adnl_, &adnl::Adnl::check_id_exists, local_id_, [SelfId = actor_id(this)](td::Result<bool> R) {
        if (R.is_ok() && R.ok()) {
          td::actor::send_closure(SelfId, &FullNodePrivateOverlayV2::init);
        } else {
          delay_action([SelfId]() { td::actor::send_closure(SelfId, &FullNodePrivateOverlayV2::try_init); },
                       td::Timestamp::in(30.0));
        }
      });
}

void FullNodePrivateOverlayV2::init() {
  LOG(FULL_NODE_INFO) << "Creating private block overlay for shard " << shard_.to_str() << ", adnl_id=" << local_id_
                      << " : " << nodes_.size() << " nodes";
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodePrivateOverlayV2::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
    }
    void get_stats_extra(td::Promise<std::string> promise) override {
      td::actor::send_closure(node_, &FullNodePrivateOverlayV2::get_stats_extra, std::move(promise));
    }
    Callback(td::actor::ActorId<FullNodePrivateOverlayV2> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodePrivateOverlayV2> node_;
  };

  std::map<PublicKeyHash, td::uint32> authorized_keys;
  for (const adnl::AdnlNodeIdShort &sender : senders_) {
    authorized_keys[sender.pubkey_hash()] = overlay::Overlays::max_fec_broadcast_size();
  }
  overlay::OverlayPrivacyRules rules{overlay::Overlays::max_fec_broadcast_size(), 0, std::move(authorized_keys)};
  std::string scope = PSTRING() << R"({ "type": "private-blocks-v2", "shard_id": )" << shard_.shard
                                << ", \"workchain_id\": " << shard_.workchain << " }";
  td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay, local_id_, overlay_id_full_.clone(),
                          nodes_, std::make_unique<Callback>(actor_id(this)), rules, std::move(scope));

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_);
  inited_ = true;
}

void FullNodePrivateOverlayV2::tear_down() {
  if (inited_) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, local_id_, overlay_id_);
  }
}

void FullNodePrivateOverlayV2::get_stats_extra(td::Promise<std::string> promise) {
  auto res = create_tl_object<ton_api::engine_validator_privateBlockOverlayV2Stats>();
  res->shard_ = shard_.to_str();
  for (const auto &x : nodes_) {
    res->nodes_.push_back(x.bits256_value());
  }
  for (const auto &x : senders_) {
    res->senders_.push_back(x.bits256_value());
  }
  res->created_at_ = created_at_;
  promise.set_result(td::json_encode<std::string>(td::ToJson(*res), true));
}

td::actor::ActorId<FullNodePrivateOverlayV2> FullNodePrivateBlockOverlaysV2::choose_overlay(ShardIdFull shard) {
  for (auto &p : id_to_overlays_) {
    auto &overlays = p.second.overlays_;
    ShardIdFull cur_shard = shard;
    while (true) {
      auto it = overlays.find(cur_shard);
      if (it != overlays.end() && it->second.is_sender_) {
        return it->second.overlay_.get();
      }
      if (cur_shard.pfx_len() == 0) {
        break;
      }
      cur_shard = shard_parent(cur_shard);
    }
  }
  return {};
}

void FullNodePrivateBlockOverlaysV2::update_overlays(
    td::Ref<MasterchainState> state, std::set<adnl::AdnlNodeIdShort> my_adnl_ids, const FileHash &zero_state_file_hash,
    const td::actor::ActorId<keyring::Keyring> &keyring, const td::actor::ActorId<adnl::Adnl> &adnl,
    const td::actor::ActorId<rldp::Rldp> &rldp, const td::actor::ActorId<rldp2::Rldp> &rldp2,
    const td::actor::ActorId<overlay::Overlays> &overlays,
    const td::actor::ActorId<ValidatorManagerInterface> &validator_manager,
    const td::actor::ActorId<FullNode> &full_node) {
  if (my_adnl_ids.empty()) {
    id_to_overlays_.clear();
    return;
  }
  auto collators = state->get_collator_config(true);
  auto all_validators = state->get_total_validator_set(0);

  struct OverlayInfo {
    std::vector<adnl::AdnlNodeIdShort> nodes, senders;
  };
  std::map<ShardIdFull, OverlayInfo> overlay_infos;

  // Masterchain overlay: all validators + collators
  OverlayInfo &mc_overlay = overlay_infos[ShardIdFull(masterchainId)];
  for (const auto &x : all_validators->export_vector()) {
    td::Bits256 addr = x.addr.is_zero() ? ValidatorFullId(x.key).compute_short_id().bits256_value() : x.addr;
    mc_overlay.nodes.emplace_back(addr);
    mc_overlay.senders.emplace_back(addr);
  }
  for (const auto &x : collators.collator_nodes) {
    mc_overlay.nodes.emplace_back(x.adnl_id);
  }

  // Shard overlays: validators of the shard + collators of the shard
  // See ValidatorManagerImpl::update_shards
  std::set<ShardIdFull> new_shards;
  for (auto &v : state->get_shards()) {
    ShardIdFull shard = v->shard();
    if (shard.is_masterchain()) {
      continue;
    }
    if (v->before_split()) {
      ShardIdFull l_shard{shard.workchain, shard_child(shard.shard, true)};
      ShardIdFull r_shard{shard.workchain, shard_child(shard.shard, false)};
      new_shards.insert(l_shard);
      new_shards.insert(r_shard);
    } else if (v->before_merge()) {
      ShardIdFull p_shard{shard.workchain, shard_parent(shard.shard)};
      new_shards.insert(p_shard);
    } else {
      new_shards.insert(shard);
    }
  }
  for (ShardIdFull shard : new_shards) {
    auto val_set = state->get_validator_set(shard);
    td::uint32 min_split = state->monitor_min_split_depth(shard.workchain);
    OverlayInfo &overlay =
        overlay_infos[shard_prefix_length(shard) <= min_split ? shard : shard_prefix(shard, min_split)];
    for (const auto &x : val_set->export_vector()) {
      td::Bits256 addr = x.addr.is_zero() ? ValidatorFullId(x.key).compute_short_id().bits256_value() : x.addr;
      overlay.nodes.emplace_back(addr);
      overlay.senders.emplace_back(addr);
    }
  }
  for (auto &p : overlay_infos) {
    ShardIdFull shard = p.first;
    OverlayInfo &overlay = p.second;
    if (!shard.is_masterchain()) {
      for (const auto &collator : collators.collator_nodes) {
        if (shard_intersects(collator.shard, shard)) {
          overlay.nodes.emplace_back(collator.adnl_id);
        }
      }
    }

    std::sort(overlay.nodes.begin(), overlay.nodes.end());
    overlay.nodes.erase(std::unique(overlay.nodes.begin(), overlay.nodes.end()), overlay.nodes.end());
    std::sort(overlay.senders.begin(), overlay.senders.end());
    overlay.senders.erase(std::unique(overlay.senders.begin(), overlay.senders.end()), overlay.senders.end());
  }

  std::map<adnl::AdnlNodeIdShort, Overlays> old_private_block_overlays = std::move(id_to_overlays_);
  id_to_overlays_.clear();

  for (const auto &p : overlay_infos) {
    ShardIdFull shard = p.first;
    const OverlayInfo &new_overlay_info = p.second;
    for (adnl::AdnlNodeIdShort local_id : new_overlay_info.nodes) {
      if (!my_adnl_ids.count(local_id)) {
        continue;
      }
      Overlays::ShardOverlay &new_overlay = id_to_overlays_[local_id].overlays_[shard];
      Overlays::ShardOverlay &old_overlay = old_private_block_overlays[local_id].overlays_[shard];
      if (!old_overlay.overlay_.empty() && old_overlay.nodes_ == new_overlay_info.nodes &&
          old_overlay.senders_ == new_overlay_info.senders) {
        new_overlay = std::move(old_overlay);
        old_overlay = {};
      } else {
        new_overlay.nodes_ = new_overlay_info.nodes;
        new_overlay.senders_ = new_overlay_info.senders;
        new_overlay.is_sender_ = std::binary_search(new_overlay.senders_.begin(), new_overlay.senders_.end(), local_id);
        new_overlay.overlay_ = td::actor::create_actor<FullNodePrivateOverlayV2>(
            PSTRING() << "BlocksPrivateOverlay" << shard.to_str(), local_id, shard, new_overlay.nodes_,
            new_overlay.senders_, zero_state_file_hash, keyring, adnl, rldp, rldp2, overlays, validator_manager,
            full_node);
      }
    }
  }

  // Delete old overlays, but not immediately
  for (auto &p : old_private_block_overlays) {
    for (auto &x : p.second.overlays_) {
      if (x.second.overlay_.empty()) {
        continue;
      }
      td::actor::ActorId<FullNodePrivateOverlayV2> id = x.second.overlay_.release();
      delay_action([id = std::move(id)]() { td::actor::send_closure(id, &FullNodePrivateOverlayV2::destroy); },
                   td::Timestamp::in(30.0));
    }
  }
}

void FullNodePrivateBlockOverlaysV2::destroy_overlays() {
  id_to_overlays_.clear();
}


}  // namespace ton::validator::fullnode