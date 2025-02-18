/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
#include <graphene/protocol/base.hpp>
#include <graphene/protocol/account.hpp>
#include <graphene/protocol/assert.hpp>
#include <graphene/protocol/asset_ops.hpp>
#include <graphene/protocol/balance.hpp>
#include <graphene/protocol/custom.hpp>
#include <graphene/protocol/committee_member.hpp>
#include <graphene/protocol/confidential.hpp>
#include <graphene/protocol/custom_authority.hpp>
#include <graphene/protocol/fba.hpp>
#include <graphene/protocol/market.hpp>
#include <graphene/protocol/proposal.hpp>
#include <graphene/protocol/ticket.hpp>
#include <graphene/protocol/transfer.hpp>
#include <graphene/protocol/vesting.hpp>
#include <graphene/protocol/withdraw_permission.hpp>
#include <graphene/protocol/witness.hpp>
#include <graphene/protocol/worker.hpp>
#include <graphene/protocol/htlc.hpp>
#include <graphene/protocol/personal_data.hpp>
#include <graphene/protocol/personal_data_v2.hpp>
#include <graphene/protocol/content_card.hpp>
#include <graphene/protocol/content_card_v2.hpp>
#include <graphene/protocol/permission.hpp>
#include <graphene/protocol/content_vote.hpp>
#include <graphene/protocol/commit_reveal.hpp>
#include <graphene/protocol/commit_reveal_v2.hpp>
#include <graphene/protocol/commit_reveal_v3.hpp>

namespace graphene { namespace protocol {

   /**
    * @ingroup operations
    *
    * Defines the set of valid operations as a discriminated union type.
    */
   typedef fc::static_variant<
            /*  0 */ transfer_operation,
            /*  1 */ account_create_operation,
            /*  2 */ account_update_operation,
            /*  3 */ account_whitelist_operation,
            /*  4 */ account_upgrade_operation,
            /*  5 */ account_transfer_operation,
            /*  6 */ asset_create_operation,
            /*  7 */ asset_update_operation,
            /*  8 */ asset_update_bitasset_operation,
            /*  9 */ asset_update_feed_producers_operation,
            /* 10 */ asset_issue_operation,
            /* 11 */ asset_reserve_operation,
            /* 12 */ asset_fund_fee_pool_operation,
            /* 13 */ asset_settle_operation,
            /* 14 */ asset_global_settle_operation,
            /* 15 */ asset_publish_feed_operation,
            /* 16 */ witness_create_operation,
            /* 17 */ witness_update_operation,
            /* 18 */ proposal_create_operation,
            /* 19 */ proposal_update_operation,
            /* 20 */ proposal_delete_operation,
            /* 21 */ withdraw_permission_create_operation,
            /* 22 */ withdraw_permission_update_operation,
            /* 23 */ withdraw_permission_claim_operation,
            /* 24 */ withdraw_permission_delete_operation,
            /* 25 */ committee_member_create_operation,
            /* 26 */ committee_member_update_operation,
            /* 27 */ committee_member_update_global_parameters_operation,
            /* 28 */ vesting_balance_create_operation,
            /* 29 */ vesting_balance_withdraw_operation,
            /* 30 */ custom_operation,
            /* 31 */ assert_operation,
            /* 32 */ balance_claim_operation,
            /* 33 */ override_transfer_operation,
            /* 34 */ transfer_to_blind_operation,
            /* 35 */ blind_transfer_operation,
            /* 36 */ transfer_from_blind_operation,
            /* 37 */ asset_settle_cancel_operation,  // VIRTUAL
            /* 38 */ asset_claim_fees_operation,
            /* 39 */ fba_distribute_operation,       // VIRTUAL
            /* 40 */ asset_claim_pool_operation,
            /* 41 */ asset_update_issuer_operation,
            /* 42 */ custom_authority_create_operation,
            /* 43 */ custom_authority_update_operation,
            /* 44 */ custom_authority_delete_operation,
            /* 45 */ ticket_create_operation,
            /* 46 */ ticket_update_operation,
            /* 47 */ personal_data_create_operation,
            /* 48 */ personal_data_remove_operation,
            /* 49 */ content_card_create_operation,
            /* 50 */ content_card_update_operation,
            /* 51 */ content_card_remove_operation,
            /* 52 */ permission_create_operation,
            /* 53 */ permission_remove_operation,
            /* 54 */ content_vote_create_operation,
            /* 55 */ content_vote_remove_operation,
            /* 56 */ vote_counter_update_operation,
            /* 57 */ commit_create_operation,
            /* 58 */ reveal_create_operation,
            /* 59 */ commit_create_v2_operation,
            /* 60 */ reveal_create_v2_operation,
            /* 61 */ commit_create_v3_operation,
            /* 62 */ reveal_create_v3_operation,
            /* 63 */ content_card_v2_create_operation,
            /* 64 */ content_card_v2_update_operation,
            /* 65 */ content_card_v2_remove_operation,
            /* 66 */ personal_data_v2_create_operation,
            /* 67 */ personal_data_v2_remove_operation,
            /* 68 */ worker_create_operation,
            /* 69 */ htlc_create_operation,
            /* 70 */ htlc_redeem_operation,
            /* 71 */ htlc_redeemed_operation,         // VIRTUAL
            /* 72 */ htlc_extend_operation,
            /* 73 */ htlc_refund_operation,           // VIRTUAL
            /* 74 */ limit_order_create_operation,
            /* 75 */ limit_order_cancel_operation,
            /* 76 */ call_order_update_operation,
            /* 77 */ fill_order_operation            // VIRTUAL
         > operation;

   /// @} // operations group

   /**
    *  Appends required authorites to the result vector.  The authorities appended are not the
    *  same as those returned by get_required_auth 
    *
    *  @return a set of required authorities for @p op
    */
   void operation_get_required_authorities( const operation& op,
                                            flat_set<account_id_type>& active,
                                            flat_set<account_id_type>& owner,
                                            vector<authority>& other,
                                            bool ignore_custom_operation_required_auths );

   void operation_validate( const operation& op );

   /**
    *  @brief necessary to support nested operations inside the proposal_create_operation
    */
   struct op_wrapper
   {
      public:
         op_wrapper(const operation& op = operation()):op(op){}
         operation op;
   };

} } // graphene::protocol

FC_REFLECT_TYPENAME( graphene::protocol::operation )
FC_REFLECT( graphene::protocol::op_wrapper, (op) )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::op_wrapper )
