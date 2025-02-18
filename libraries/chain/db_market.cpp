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

#include <graphene/chain/database.hpp>

#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/is_authorized_asset.hpp>

#include <fc/uint128.hpp>

namespace graphene { namespace chain {

namespace detail {

   share_type calculate_percent(const share_type& value, uint16_t percent)
   {
      fc::uint128_t a(value.value);
      a *= percent;
      a /= GRAPHENE_100_PERCENT;
      FC_ASSERT( a <= GRAPHENE_MAX_SHARE_SUPPLY, "overflow when calculating percent" );
      return static_cast<int64_t>(a);
   }

} //detail

/**
 * All margin positions are force closed at the swan price
 * Collateral received goes into a force-settlement fund
 * No new margin positions can be created for this asset
 * Force settlement happens without delay at the swan price, deducting from force-settlement fund
 * No more asset updates may be issued.
*/
void database::globally_settle_asset( const asset_object& mia, const price& settlement_price )
{
   globally_settle_asset_impl( mia, settlement_price,
                               get_index_type<call_order_index>().indices().get<by_collateral>() );
}

template<typename IndexType>
void database::globally_settle_asset_impl( const asset_object& mia,
                                           const price& settlement_price,
                                           const IndexType& call_index )
{ try {
   const asset_bitasset_data_object& bitasset = mia.bitasset_data(*this);
   FC_ASSERT( !bitasset.has_settlement(), "black swan already occurred, it should not happen again" );

   const asset_object& backing_asset = bitasset.options.short_backing_asset(*this);
   asset collateral_gathered = backing_asset.amount(0);

   const asset_dynamic_data_object& mia_dyn = mia.dynamic_asset_data_id(*this);
   auto original_mia_supply = mia_dyn.current_supply;

   // cancel all call orders and accumulate it into collateral_gathered
   auto call_itr = call_index.lower_bound( price::min( bitasset.options.short_backing_asset, mia.id ) );
   auto call_end = call_index.upper_bound( price::max( bitasset.options.short_backing_asset, mia.id ) );

   asset pays;
   while( call_itr != call_end )
   {
      const call_order_object& order = *call_itr;
      ++call_itr;

      pays = order.get_debt().multiply_and_round_up( settlement_price ); // round up in favor of global-settle fund

      if( pays > order.get_collateral() )
         pays = order.get_collateral();

      collateral_gathered += pays;

      FC_ASSERT( fill_call_order( order, pays, order.get_debt(), settlement_price, true ) ); // call order is maker
   }

   modify( bitasset, [&mia,original_mia_supply,&collateral_gathered]( asset_bitasset_data_object& obj ){
           obj.settlement_price = mia.amount(original_mia_supply) / collateral_gathered;
           obj.settlement_fund  = collateral_gathered.amount;
           });

   /// After all margin positions are closed, the current supply will be reported as 0, but
   /// that is a lie, the supply didn't change.   We need to capture the current supply before
   /// filling all call orders and then restore it afterward.   Then in the force settlement
   /// evaluator reduce the supply
   modify( mia_dyn, [original_mia_supply]( asset_dynamic_data_object& obj ){
           obj.current_supply = original_mia_supply;
         });

} FC_CAPTURE_AND_RETHROW( (mia)(settlement_price) ) }

void database::revive_bitasset( const asset_object& bitasset )
{ try {
   FC_ASSERT( bitasset.is_market_issued() );
   const asset_bitasset_data_object& bad = bitasset.bitasset_data(*this);
   FC_ASSERT( bad.has_settlement() );
   const asset_dynamic_data_object& bdd = bitasset.dynamic_asset_data_id(*this);
   FC_ASSERT( !bad.is_prediction_market );
   FC_ASSERT( !bad.current_feed.settlement_price.is_null() );

   if( bdd.current_supply <= 0 )
      FC_ASSERT( bad.settlement_fund == 0 );

   _cancel_bids_and_revive_mpa( bitasset, bad );
} FC_CAPTURE_AND_RETHROW( (bitasset) ) }

void database::_cancel_bids_and_revive_mpa( const asset_object& bitasset, const asset_bitasset_data_object& bad )
{ try {
   FC_ASSERT( bitasset.is_market_issued() );
   FC_ASSERT( bad.has_settlement() );
   FC_ASSERT( !bad.is_prediction_market );

   // revive
   modify( bad, [&]( asset_bitasset_data_object& obj ){
              obj.settlement_price = price();
              obj.settlement_fund = 0;
           });
} FC_CAPTURE_AND_RETHROW( (bitasset) ) }

void database::cancel_settle_order(const force_settlement_object& order, bool create_virtual_op)
{
   adjust_balance(order.owner, order.balance);

   if( create_virtual_op )
   {
      asset_settle_cancel_operation vop;
      vop.settlement = order.id;
      vop.account = order.owner;
      vop.amount = order.balance;
      push_applied_operation( vop );
   }
   remove(order);
}

void database::cancel_limit_order( const limit_order_object& order, bool create_virtual_op, bool skip_cancel_fee )
{
   // if need to create a virtual op, try deduct a cancellation fee here.
   // there are two scenarios when order is cancelled and need to create a virtual op:
   // 1. due to expiration: always deduct a fee if there is any fee deferred
   // 2. due to cull_small: deduct a fee after hard fork 604, but not before (will set skip_cancel_fee)
   const account_statistics_object* seller_acc_stats = nullptr;
   const asset_dynamic_data_object* fee_asset_dyn_data = nullptr;
   limit_order_cancel_operation vop;
   share_type deferred_fee = order.deferred_fee;
   asset deferred_paid_fee = order.deferred_paid_fee;
   if( create_virtual_op )
   {
      vop.order = order.id;
      vop.fee_paying_account = order.seller;
      // only deduct fee if not skipping fee, and there is any fee deferred
      if( !skip_cancel_fee && deferred_fee > 0 )
      {
         asset core_cancel_fee = current_fee_schedule().calculate_fee( vop );
         // cap the fee
         if( core_cancel_fee.amount > deferred_fee )
            core_cancel_fee.amount = deferred_fee;
         // if there is any CORE fee to deduct, redirect it to referral program
         if( core_cancel_fee.amount > 0 )
         {
            seller_acc_stats = &order.seller( *this ).statistics( *this );
            modify( *seller_acc_stats, [&]( account_statistics_object& obj ) {
               obj.pay_fee( core_cancel_fee.amount, get_global_properties().parameters.cashback_vesting_threshold );
            } );
            deferred_fee -= core_cancel_fee.amount;
            // handle originally paid fee if any:
            //    to_deduct = round_up( paid_fee * core_cancel_fee / deferred_core_fee_before_deduct )
            if( deferred_paid_fee.amount == 0 )
            {
               vop.fee = core_cancel_fee;
            }
            else
            {
               fc::uint128_t fee128( deferred_paid_fee.amount.value );
               fee128 *= core_cancel_fee.amount.value;
               // to round up
               fee128 += order.deferred_fee.value;
               fee128 -= 1;
               fee128 /= order.deferred_fee.value;
               share_type cancel_fee_amount = static_cast<int64_t>(fee128);
               // cancel_fee should be positive, pay it to asset's accumulated_fees
               fee_asset_dyn_data = &deferred_paid_fee.asset_id(*this).dynamic_asset_data_id(*this);
               modify( *fee_asset_dyn_data, [&](asset_dynamic_data_object& addo) {
                  addo.accumulated_fees += cancel_fee_amount;
               });
               // cancel_fee should be no more than deferred_paid_fee
               deferred_paid_fee.amount -= cancel_fee_amount;
               vop.fee = asset( cancel_fee_amount, deferred_paid_fee.asset_id );
            }
         }
      }
   }

   // refund funds in order
   auto refunded = order.amount_for_sale();
   if( refunded.asset_id == asset_id_type() )
   {
      if( seller_acc_stats == nullptr )
         seller_acc_stats = &order.seller( *this ).statistics( *this );
      modify( *seller_acc_stats, [&]( account_statistics_object& obj ) {
         obj.total_core_in_orders -= refunded.amount;
      });
   }
   adjust_balance(order.seller, refunded);

   // refund fee
   // could be virtual op or real op here
   if( order.deferred_paid_fee.amount == 0 )
   {
      // be here, fee paid in CORE, or no fee to refund.
      // if order was created before hard fork 604 then cancelled no matter before or after hard fork 604,
      //    see it as fee paid in CORE, deferred_fee should be refunded to order owner but not fee pool
      adjust_balance( order.seller, deferred_fee );
   }
   else // need to refund fee in originally paid asset
   {
      adjust_balance(order.seller, deferred_paid_fee);
      // be here, must have: fee_asset != CORE
      if( fee_asset_dyn_data == nullptr )
         fee_asset_dyn_data = &deferred_paid_fee.asset_id(*this).dynamic_asset_data_id(*this);
      modify( *fee_asset_dyn_data, [&](asset_dynamic_data_object& addo) {
         addo.fee_pool += deferred_fee;
      });
   }

   if( create_virtual_op )
      push_applied_operation( vop );

   remove(order);
}

bool maybe_cull_small_order( database& db, const limit_order_object& order )
{
   /**
    *  There are times when the AMOUNT_FOR_SALE * SALE_PRICE == 0 which means that we
    *  have hit the limit where the seller is asking for nothing in return.  When this
    *  happens we must refund any balance back to the seller, it is too small to be
    *  sold at the sale price.
    *
    *  If the order is a taker order (as opposed to a maker order), so the price is
    *  set by the counterparty, this check is deferred until the order becomes unmatched
    *  (see #555) -- however, detecting this condition is the responsibility of the caller.
    */
   if( order.amount_to_receive().amount == 0 )
   {
      db.cancel_limit_order( order );
      return true;
   }
   return false;
}

/***
 * @brief apply a new limit_order_object to the market, matching with existing limit orders or
 *    margin call orders where possible, leaving remainder on the book if not fully matched.
 * @detail Called from limit_order_create_evaluator::do_apply() in market_evaluator.cpp in
 *    response to a limit_order_create operation.  If we're not at the front of the book, we
 *    return false early and do nothing else, since there's nothing we can match.  If we are at
 *    the front of the book, then we first look for matching limit orders that are more
 *    favorable than the margin call price, then we search through active margin calls, then
 *    finaly the remaining limit orders, until we either fully consume the order or can no
 *    longer match and must leave the remainder on the book.
 * @return Returns true if limit order is completely consumed by matching, else false if it
 *    remains on the book.
 * @param new_order_object the new limit order (read only ref, though the corresponding db
 *    object is modified as we match and deleted if filled completely)
 * @param allow_black_swan ignored, defaulted to true
 */
bool database::apply_order(const limit_order_object& new_order_object, bool allow_black_swan)
{
   auto order_id = new_order_object.id;
   asset_id_type sell_asset_id = new_order_object.sell_asset_id();
   asset_id_type recv_asset_id = new_order_object.receive_asset_id();

   // We only need to check if the new order will match with others if it is at the front of the book
   const auto& limit_price_idx = get_index_type<limit_order_index>().indices().get<by_price>();
   auto limit_itr = limit_price_idx.lower_bound( boost::make_tuple( new_order_object.sell_price, order_id ) );
   if( limit_itr != limit_price_idx.begin() )
   {
      --limit_itr;
      if( limit_itr->sell_asset_id() == sell_asset_id && limit_itr->receive_asset_id() == recv_asset_id )
         return false;
   }

   // this is the opposite side (on the book)
   auto max_price = ~new_order_object.sell_price;
   limit_itr = limit_price_idx.lower_bound( max_price.max() );
   auto limit_end = limit_price_idx.upper_bound( max_price );

   // Order matching should be in favor of the taker.
   // When a new limit order is created, e.g. an ask, need to check if it will match the highest bid.
   // We were checking call orders first. However, due to MSSR (maximum_short_squeeze_ratio),
   // effective price of call orders may be worse than limit orders, so we should also check limit orders here.

   // Question: will a new limit order trigger a black swan event?
   //
   // 1. as of writing, it's possible due to the call-order-and-limit-order overlapping issue:
   //       https://github.com/bitshares/bitshares-core/issues/606 .
   //    when it happens, a call order can be very big but don't match with the opposite,
   //    even when price feed is too far away, further than swan price,
   //    if the new limit order is in the same direction with the call orders, it can eat up all the opposite,
   //    then the call order will lose support and trigger a black swan event.
   // 2. after issue 606 is fixed, there will be no limit order on the opposite side "supporting" the call order,
   //    so a new order in the same direction with the call order won't trigger a black swan event.
   // 3. calling is one direction. if the new limit order is on the opposite direction,
   //    no matter if matches with the call, it won't trigger a black swan event.
   //    (if a match at MSSP caused a black swan event, it means the call order is already undercollateralized,
   //      which should trigger a black swan event earlier.)
   //
   // Since it won't trigger a black swan, no need to check here.

   // currently we don't do cross-market (triangle) matching.
   // the limit order will only match with a call order if meet all of these:
   // 1. it's buying collateral, which means sell_asset is the MIA, receive_asset is the backing asset.
   // 2. sell_asset is not a prediction market
   // 3. sell_asset is not globally settled
   // 4. sell_asset has a valid price feed
   // 5. the call order's collateral ratio is below or equals to MCR
   // 6. the limit order provided a good price

   bool to_check_call_orders = false;
   const asset_object& sell_asset = sell_asset_id( *this );
   const asset_bitasset_data_object* sell_abd = nullptr;
   price call_match_price;  // Price at which margin calls sit on the books. Prior to BSIP-74 this price is
                            // same as the MSSP. After, it is the MCOP, which may deviate from MSSP due to MCFR.
   price call_pays_price;   // Price margin call actually relinquishes collateral at. Equals the MSSP and it may
                            // differ from call_match_price if there is a Margin Call Fee.
   if( sell_asset.is_market_issued() )
   {
      sell_abd = &sell_asset.bitasset_data( *this );
      if( sell_abd->options.short_backing_asset == recv_asset_id
          && !sell_abd->is_prediction_market
          && !sell_abd->has_settlement()
          && !sell_abd->current_feed.settlement_price.is_null() )
      {
         call_match_price = ~sell_abd->current_feed.
            margin_call_order_price(sell_abd->options.extensions.value.margin_call_fee_ratio);
         call_pays_price = ~sell_abd->current_feed.max_short_squeeze_price();
         if( ~new_order_object.sell_price <= call_match_price ) // If new limit order price is good enough to
            to_check_call_orders = true;                        // match a call, then check if there are calls.
      }
   }

   bool finished = false; // whether the new order is gone
   if( to_check_call_orders )
   {
      // check limit orders first, match the ones with better price in comparison to call orders
      while( !finished && limit_itr != limit_end && limit_itr->sell_price > call_match_price )
      {
         auto old_limit_itr = limit_itr;
         ++limit_itr;
         // match returns 2 when only the old order was fully filled. In this case, we keep matching; otherwise, we stop.
         finished = ( match( new_order_object, *old_limit_itr, old_limit_itr->sell_price ) != 2 );
      }

      if( !finished ) // TODO refactor or cleanup duplicate code
      {
         // check if there are margin calls
         const auto& call_collateral_idx = get_index_type<call_order_index>().indices().get<by_collateral>();
         auto call_min = price::min( recv_asset_id, sell_asset_id );
         while( !finished )
         {
            // always check call order with least collateral ratio
            auto call_itr = call_collateral_idx.lower_bound( call_min );
            if( call_itr == call_collateral_idx.end()
                  || call_itr->debt_type() != sell_asset_id
                  // feed protected https://github.com/cryptonomex/graphene/issues/436
                  || call_itr->collateralization() > sell_abd->current_maintenance_collateralization )
               break;
            int match_result = match( new_order_object, *call_itr, call_match_price,
                                      sell_abd->current_feed.settlement_price,
                                      sell_abd->current_feed.maintenance_collateral_ratio,
                                      sell_abd->current_maintenance_collateralization,
                                      call_pays_price);
            // match returns 1 or 3 when the new order was fully filled. In this case, we stop matching; otherwise keep matching.
            // since match can return 0 due to BSIP38 (hard fork core-834), we no longer only check if the result is 2.
            if( match_result == 1 || match_result == 3 )
               finished = true;
         }
      }
   }

   // still need to check limit orders
   while( !finished && limit_itr != limit_end )
   {
      auto old_limit_itr = limit_itr;
      ++limit_itr;
      // match returns 2 when only the old order was fully filled. In this case, we keep matching; otherwise, we stop.
      finished = ( match( new_order_object, *old_limit_itr, old_limit_itr->sell_price ) != 2 );
   }

   const limit_order_object* updated_order_object = find< limit_order_object >( order_id );
   if( updated_order_object == nullptr )
      return true;

   // before #555 we would have done maybe_cull_small_order() logic as a result of fill_order() being called by match() above
   // however after #555 we need to get rid of small orders -- #555 hardfork defers logic that was done too eagerly before, and
   // this is the point it's deferred to.
   return maybe_cull_small_order( *this, *updated_order_object );
}

/**
 *  Matches the two orders, the first parameter is taker, the second is maker.
 *
 *  @return a bit field indicating which orders were filled (and thus removed)
 *
 *  0 - no orders were matched
 *  1 - taker was filled
 *  2 - maker was filled
 *  3 - both were filled
 */
int database::match( const limit_order_object& usd, const limit_order_object& core, const price& match_price )
{
   FC_ASSERT( usd.sell_price.quote.asset_id == core.sell_price.base.asset_id );
   FC_ASSERT( usd.sell_price.base.asset_id  == core.sell_price.quote.asset_id );
   FC_ASSERT( usd.for_sale > 0 && core.for_sale > 0 );

   auto usd_for_sale = usd.amount_for_sale();
   auto core_for_sale = core.amount_for_sale();

   asset usd_pays, usd_receives, core_pays, core_receives;

   bool cull_taker = false;
   if( usd_for_sale <= core_for_sale * match_price ) // rounding down here should be fine
   {
      usd_receives  = usd_for_sale * match_price; // round down, in favor of bigger order

      // Be here, it's possible that taker is paying something for nothing due to partially filled in last loop.
      // In this case, we see it as filled and cancel it later
      if( usd_receives.amount == 0 )
         return 1;

      // The remaining amount in order `usd` would be too small,
      //   so we should cull the order in fill_limit_order() below.
      // The order would receive 0 even at `match_price`, so it would receive 0 at its own price,
      //   so calling maybe_cull_small() will always cull it.
      core_receives = usd_receives.multiply_and_round_up( match_price );
      cull_taker = true;
   }
   else
   {
      //This line once read: assert( core_for_sale < usd_for_sale * match_price );
      //This assert is not always true -- see trade_amount_equals_zero in operation_tests.cpp
      //Although usd_for_sale is greater than core_for_sale * match_price, core_for_sale == usd_for_sale * match_price
      //Removing the assert seems to be safe -- apparently no asset is created or destroyed.

      // The maker won't be paying something for nothing, since if it would, it would have been cancelled already.
      core_receives = core_for_sale * match_price; // round down, in favor of bigger order
      // The remaining amount in order `core` would be too small,
      //   so the order will be culled in fill_limit_order() below
      usd_receives = core_receives.multiply_and_round_up( match_price );
   }

   core_pays = usd_receives;
   usd_pays  = core_receives;

   int result = 0;
   result |= fill_limit_order( usd, usd_pays, usd_receives, cull_taker, match_price, false ); // the first param is taker
   result |= fill_limit_order( core, core_pays, core_receives, true, match_price, true ) << 1; // the second param is maker
   FC_ASSERT( result != 0 );
   return result;
}

int database::match( const limit_order_object& bid, const call_order_object& ask, const price& match_price,
                     const price& feed_price, const uint16_t maintenance_collateral_ratio,
                     const optional<price>& maintenance_collateralization,
                     const price& call_pays_price )
{
   FC_ASSERT( bid.sell_asset_id() == ask.debt_type() );
   FC_ASSERT( bid.receive_asset_id() == ask.collateral_type() );
   FC_ASSERT( bid.for_sale > 0 && ask.debt > 0 && ask.collateral > 0 );

   bool cull_taker = false;

   asset usd_for_sale = bid.amount_for_sale();
   asset usd_to_buy   = asset( ask.get_max_debt_to_cover( call_pays_price, feed_price,
         maintenance_collateral_ratio,  maintenance_collateralization ), ask.debt_type() );

   asset call_pays, call_receives, order_pays, order_receives;
   if( usd_to_buy > usd_for_sale )
   {  // fill limit order
      order_receives  = usd_for_sale * match_price; // round down here, in favor of call order
      call_pays       = usd_for_sale * call_pays_price; // (same as match_price until BSIP-74)

      // Be here, it's possible that taker is paying something for nothing due to partially filled in last loop.
      // In this case, we see it as filled and cancel it later
      if( order_receives.amount == 0 )
         return 1;

      // The remaining amount in the limit order would be too small,
      //   so we should cull the order in fill_limit_order() below.
      // The order would receive 0 even at `match_price`, so it would receive 0 at its own price,
      //   so calling maybe_cull_small() will always cull it.
      call_receives = order_receives.multiply_and_round_up( match_price );
      cull_taker = true;
   }
   else
   {  // fill call order
      call_receives  = usd_to_buy;
      order_receives = usd_to_buy.multiply_and_round_up( match_price ); // round up here, in favor of limit order
      call_pays      = usd_to_buy.multiply_and_round_up( call_pays_price );
   }
   order_pays = call_receives;

   // Compute margin call fee (BSIP74). Difference between what the call order pays and the limit order
   // receives is the margin call fee that is paid by the call order owner to the asset issuer.
   // Margin call fee should equal = X*MCFR/settle_price, to within rounding error.
   FC_ASSERT(call_pays >= order_receives);
   const asset margin_call_fee = call_pays - order_receives;

   int result = 0;
   result |= fill_limit_order( bid, order_pays, order_receives, cull_taker, match_price, false ); // taker
   result |= fill_call_order( ask, call_pays, call_receives, match_price, true, margin_call_fee ) << 1; // maker
   // result can be 0 when call order has target_collateral_ratio option set.

   return result;
}


asset database::match( const call_order_object& call, 
                       const force_settlement_object& settle, 
                       const price& match_price,
                       asset max_settlement,
                       const price& fill_price )
{ try {
   FC_ASSERT(call.get_debt().asset_id == settle.balance.asset_id );
   FC_ASSERT(call.debt > 0 && call.collateral > 0 && settle.balance.amount > 0);

   auto settle_for_sale = std::min(settle.balance, max_settlement);
   auto call_debt = call.get_debt();

   asset call_receives   = std::min(settle_for_sale, call_debt);
   asset call_pays       = call_receives * match_price; // round down here, in favor of call order, for first check
                                                        // TODO possible optimization: check need to round up or down first

   // Be here, the call order may be paying nothing.
   bool cull_settle_order = false; // whether need to cancel dust settle order
   if( call_pays.amount == 0 )
   {
      if( call_receives == call_debt ) // the call order is smaller than or equal to the settle order
      {
         call_pays.amount = 1;
      }
      else
      {
         if( call_receives == settle.balance ) // the settle order is smaller
         {
            cancel_settle_order( settle );
         }
         // else do nothing: neither order will be completely filled, perhaps due to max_settlement too small

         return asset( 0, settle.balance.asset_id );
      }

   }
   else // the call order is not paying nothing, but still possible it's paying more than minimum required due to rounding
   {
      if( call_receives == call_debt ) // the call order is smaller than or equal to the settle order
      {
         call_pays = call_receives.multiply_and_round_up( match_price ); // round up here, in favor of settle order
         // be here, we should have: call_pays <= call_collateral
      }
      else
      {
         // be here, call_pays has been rounded down

         // be here, we should have: call_pays <= call_collateral

         if( call_receives == settle.balance ) // the settle order will be completely filled, assuming we need to cull it
            cull_settle_order = true;
         // else do nothing, since we can't cull the settle order

         call_receives = call_pays.multiply_and_round_up( match_price ); // round up here to mitigate rounding issue.
                                                                           // It is important to understand here that the newly
                                                                           // rounded up call_receives won't be greater than the
                                                                           // old call_receives.

         if( call_receives == settle.balance ) // the settle order will be completely filled, no need to cull
            cull_settle_order = false;
         // else do nothing, since we still need to cull the settle order or still can't cull the settle order
      }
   }

   asset settle_pays     = call_receives;
   asset settle_receives = call_pays;

   fill_call_order( call, call_pays, call_receives, fill_price, true ); // call order is maker
   fill_settle_order( settle, settle_pays, settle_receives, fill_price, false ); // force settlement order is taker

   if( cull_settle_order )
      cancel_settle_order( settle );

   return call_receives;
} FC_CAPTURE_AND_RETHROW( (call)(settle)(match_price)(max_settlement) ) }

bool database::fill_limit_order( const limit_order_object& order, const asset& pays, const asset& receives, bool cull_if_small,
                           const price& fill_price, const bool is_maker)
{ try {
   FC_ASSERT( order.amount_for_sale().asset_id == pays.asset_id );
   FC_ASSERT( pays.asset_id != receives.asset_id );

   const account_object& seller = order.seller(*this);

   const auto issuer_fees = pay_market_fees(&seller, receives.asset_id(*this), receives, is_maker);

   pay_order( seller, receives - issuer_fees, pays );

   assert( pays.asset_id != receives.asset_id );
   push_applied_operation( fill_order_operation( order.id, order.seller, pays, receives, issuer_fees, fill_price, is_maker ) );

   // BSIP85: Maker order creation fee discount, https://github.com/bitshares/bsips/blob/master/bsip-0085.md
   //   if the order creation fee was paid in RVP,
   //     return round_down(deferred_fee * maker_fee_discount_percent) to the owner,
   //     then process the remaining deferred fee as before;
   //   if the order creation fee was paid in another asset,
   //     return round_down(deferred_paid_fee * maker_fee_discount_percent) to the owner,
   //     return round_down(deferred_fee * maker_fee_discount_percent) to the fee pool of the asset,
   //     then process the remaining deferred fee and deferred paid fee as before.
   const uint16_t maker_discount_percent = get_global_properties().parameters.get_maker_fee_discount_percent();

   // Save local copies for calculation
   share_type deferred_fee = order.deferred_fee;
   share_type deferred_paid_fee = order.deferred_paid_fee.amount;

   // conditional because cheap integer comparison may allow us to avoid two expensive modify() and object lookups
   if( order.deferred_paid_fee.amount > 0 )
   {
      share_type fee_pool_refund = 0;
      if( is_maker && maker_discount_percent > 0 )
      {
         share_type refund = detail::calculate_percent( deferred_paid_fee, maker_discount_percent );
         // Note: it's possible that the deferred_paid_fee is very small,
         //       which can result in a zero refund due to rounding issue,
         //       in this case, no refund to the fee pool
         if( refund > 0 )
         {
            FC_ASSERT( refund <= deferred_paid_fee, "Internal error" );
            adjust_balance( order.seller, asset(refund, order.deferred_paid_fee.asset_id) );
            deferred_paid_fee -= refund;

            // deferred_fee might be positive too
            FC_ASSERT( deferred_fee > 0, "Internal error" );
            fee_pool_refund = detail::calculate_percent( deferred_fee, maker_discount_percent );
            FC_ASSERT( fee_pool_refund <= deferred_fee, "Internal error" );
            deferred_fee -= fee_pool_refund;
         }
      }

      const auto& fee_asset_dyn_data = order.deferred_paid_fee.asset_id(*this).dynamic_asset_data_id(*this);
      modify( fee_asset_dyn_data, [deferred_paid_fee,fee_pool_refund](asset_dynamic_data_object& addo) {
         addo.accumulated_fees += deferred_paid_fee;
         addo.fee_pool += fee_pool_refund;
      });
   }

   if( order.deferred_fee > 0 )
   {
      if( order.deferred_paid_fee.amount <= 0 // paid in CORE, or before HF 604
            && is_maker && maker_discount_percent > 0 )
      {
         share_type refund = detail::calculate_percent( deferred_fee, maker_discount_percent );
         if( refund > 0 )
         {
            FC_ASSERT( refund <= deferred_fee, "Internal error" );
            adjust_balance( order.seller, asset(refund, asset_id_type()) );
            deferred_fee -= refund;
         }
      }
      // else do nothing here, because we have already processed it above, or no need to process

      if( deferred_fee > 0 )
      {
         modify( seller.statistics(*this), [deferred_fee,this]( account_statistics_object& statistics )
         {
            statistics.pay_fee( deferred_fee, get_global_properties().parameters.cashback_vesting_threshold );
         } );
      }
   }

   if( pays == order.amount_for_sale() )
   {
      remove( order );
      return true;
   }
   else
   {
      modify( order, [&pays]( limit_order_object& b ) {
                             b.for_sale -= pays.amount;
                             b.deferred_fee = 0;
                             b.deferred_paid_fee.amount = 0;
                          });
      if( cull_if_small )
         return maybe_cull_small_order( *this, order );
      return false;
   }
} FC_CAPTURE_AND_RETHROW( (order)(pays)(receives) ) }

/***
 * @brief fill a call order in the specified amounts
 * @param order the call order
 * @param pays What the call order will give to the other party (collateral)
 * @param receives what the call order will receive from the other party (debt)
 * @param fill_price the price at which the call order will execute
 * @param is_maker TRUE if the call order is the maker, FALSE if it is the taker
 * @param margin_call_fee Margin call fees paid in collateral asset
 * @returns TRUE if the call order was completely filled
 */
bool database::fill_call_order( const call_order_object& order, const asset& pays, const asset& receives,
      const price& fill_price, const bool is_maker, const asset& margin_call_fee )
{ try {
   FC_ASSERT( order.debt_type() == receives.asset_id );
   FC_ASSERT( order.collateral_type() == pays.asset_id );
   FC_ASSERT( order.collateral >= pays.amount );

   // TODO pass in mia and bitasset_data for better performance
   const asset_object& mia = receives.asset_id(*this);
   FC_ASSERT( mia.is_market_issued() );

   optional<asset> collateral_freed;
   // adjust the order
   modify( order, [&]( call_order_object& o ) {
         o.debt       -= receives.amount;
         o.collateral -= pays.amount;
         if( o.debt == 0 ) // is the whole debt paid?
         {
            collateral_freed = o.get_collateral();
            o.collateral = 0;
         }
      });

   // update current supply
   const asset_dynamic_data_object& mia_ddo = mia.dynamic_asset_data_id(*this);
   modify( mia_ddo, [&receives]( asset_dynamic_data_object& ao ){
         ao.current_supply -= receives.amount;
      });

   // If the whole debt is paid, adjust borrower's collateral balance
   if( collateral_freed.valid() )
      adjust_balance( order.borrower, *collateral_freed );

   // Update account statistics. We know that order.collateral_type() == pays.asset_id
   if( pays.asset_id == asset_id_type() )
   {
      modify( get_account_stats_by_owner(order.borrower), [&collateral_freed,&pays]( account_statistics_object& b ){
         b.total_core_in_orders -= pays.amount;
         if( collateral_freed.valid() )
            b.total_core_in_orders -= collateral_freed->amount;
      });
   }

   // BSIP74: Accumulate the collateral-denominated fee
   if (margin_call_fee.amount.value != 0)
      mia.accumulate_fee(*this, margin_call_fee);

   // virtual operation for account history
   push_applied_operation( fill_order_operation( order.id, order.borrower, pays, receives,
         margin_call_fee, fill_price, is_maker ) );

   // Call order completely filled, remove it
   if( collateral_freed.valid() )
      remove( order );

   return collateral_freed.valid();
} FC_CAPTURE_AND_RETHROW( (order)(pays)(receives) ) }

/***
 * @brief fullfill a settle order in the specified amounts
 *
 * @details Called from database::match(), this coordinates exchange of debt asset X held in the
 *    settle order for collateral asset Y held in a call order, and routes fees.  Note that we
 *    don't touch the call order directly, as match() handles this via a separate call to
 *    fill_call_order().  We are told exactly how much X and Y to exchange, based on details of
 *    order matching determined higher up the call chain. Thus it is possible that the settle
 *    order is not completely satisfied at the conclusion of this function.
 *
 * @param settle the force_settlement object
 * @param pays the quantity of market-issued debt asset X which the settler will yield in this
 *    round (may be less than the full amount indicated in settle object)
 * @param receives the quantity of collateral asset Y which the settler will receive in
 *    exchange for X
 * @param fill_price the price at which the settle order will execute (not used - passed through
 *    to virtual operation)
 * @param is_maker TRUE if the settle order is the maker, FALSE if it is the taker (passed
 *    through to virtual operation)
 * @returns TRUE if the settle order was completely filled, FALSE if only partially filled
 */
bool database::fill_settle_order( const force_settlement_object& settle, const asset& pays, const asset& receives,
                                  const price& fill_price, const bool is_maker )
{ try {
   bool filled = false;

   const account_object* settle_owner_ptr = nullptr;
   // The owner of the settle order pays market fees to the issuer of the collateral asset.
   // After HF core-1780, these fees are shared to the referral program, which is flagged to
   // pay_market_fees by setting settle_owner_ptr non-null.
   settle_owner_ptr = &settle.owner(*this);
   // Compute and pay the market fees:
   asset market_fees = pay_market_fees( settle_owner_ptr, get(receives.asset_id), receives, is_maker );

   // Issuer of the settled smartcoin asset lays claim to a force-settlement fee (BSIP87), but
   // note that fee is denominated in collateral asset, not the debt asset.  Asset object of
   // debt asset is passed to the pay function so it knows where to put the fee. Note that
   // amount of collateral asset upon which fee is assessed is reduced by market_fees already
   // paid to prevent the total fee exceeding total collateral.
   asset force_settle_fees = pay_force_settle_fees( get(pays.asset_id), receives - market_fees );

   auto total_collateral_denominated_fees = market_fees + force_settle_fees;

   // If we don't consume entire settle order:
   if( pays < settle.balance )
   {
      modify(settle, [&pays](force_settlement_object& s) {
         s.balance -= pays;
      });
   } else {
      filled = true;
   }
   // Give released collateral not already taken as fees to settle order owner:
   adjust_balance(settle.owner, receives - total_collateral_denominated_fees);

   assert( pays.asset_id != receives.asset_id );
   push_applied_operation( fill_order_operation( settle.id, settle.owner, pays, receives,
                                                 total_collateral_denominated_fees, fill_price, is_maker ) );

   if (filled)
      remove(settle);

   return filled;

} FC_CAPTURE_AND_RETHROW( (settle)(pays)(receives) ) }

/**
 *  Starting with the least collateralized orders, fill them if their
 *  call price is above the max(lowest bid,call_limit).
 *
 *  This method will return true if it filled a short or limit
 *
 *  @param mia - the market issued asset that should be called.
 *  @param enable_black_swan - when adjusting collateral, triggering a black swan is invalid and will throw
 *                             if enable_black_swan is not set to true.
 *  @param for_new_limit_order - true if this function is called when matching call orders with a new
 *     limit order.
 *  @param bitasset_ptr - an optional pointer to the bitasset_data object of the asset
 *
 *  @return true if a margin call was executed.
 */
bool database::check_call_orders( const asset_object& mia, bool enable_black_swan, bool for_new_limit_order,
                                  const asset_bitasset_data_object* bitasset_ptr )
{ try {
    if( for_new_limit_order )
       FC_ASSERT( false, "`for_new_limit_order` is only true before HF 338 / 625");

    if( !mia.is_market_issued() ) return false;

    const asset_bitasset_data_object& bitasset = ( bitasset_ptr ? *bitasset_ptr : mia.bitasset_data(*this) );
    
    // price feeds can cause black swans in prediction markets
    // The hardfork check may be able to be removed after the hardfork date
    // if check_for_blackswan never triggered a black swan on a prediction market.
    // NOTE: check_for_blackswan returning true does not always mean a black
    // swan was triggered.
    if ( bitasset.is_prediction_market )
       return false;

    if( check_for_blackswan( mia, enable_black_swan, &bitasset ) )
       return false;

    if( bitasset.is_prediction_market ) return false;
    if( bitasset.current_feed.settlement_price.is_null() ) return false;

    const limit_order_index& limit_index = get_index_type<limit_order_index>();
    const auto& limit_price_index = limit_index.indices().get<by_price>();

    // Looking for limit orders selling the most USD for the least CORE.
    auto max_price = price::max( mia.id, bitasset.options.short_backing_asset );
    // Stop when limit orders are selling too little USD for too much CORE.
    // Note that since BSIP74, margin calls offer somewhat less CORE per USD
    // if the issuer claims a Margin Call Fee.
    auto min_price = bitasset.current_feed.margin_call_order_price(
                           bitasset.options.extensions.value.margin_call_fee_ratio );

    // NOTE limit_price_index is sorted from greatest to least
    auto limit_itr = limit_price_index.lower_bound( max_price );
    auto limit_end = limit_price_index.upper_bound( min_price );

    if( limit_itr == limit_end )
       return false;

    const call_order_index& call_index = get_index_type<call_order_index>();
    const auto& call_collateral_index = call_index.indices().get<by_collateral>();

    auto call_min = price::min( bitasset.options.short_backing_asset, mia.id );
    auto call_max = price::max( bitasset.options.short_backing_asset, mia.id );

    auto call_collateral_itr = call_collateral_index.begin();
    auto call_collateral_end = call_collateral_itr;

    call_collateral_itr = call_collateral_index.lower_bound( call_min );
    call_collateral_end = call_collateral_index.upper_bound( call_max );

    bool margin_called = false;         // toggles true once/if we actually execute a margin call

    auto head_num = head_block_num();

    while( !check_for_blackswan( mia, enable_black_swan, &bitasset ) // TODO perhaps improve performance by passing in iterators
           && limit_itr != limit_end
           && ( call_collateral_itr != call_collateral_end ) )
    {
       const call_order_object& call_order = *call_collateral_itr;

       // Feed protected (don't call if CR>MCR) https://github.com/cryptonomex/graphene/issues/436
       if( ( bitasset.current_maintenance_collateralization < call_order.collateralization() ) )
          return margin_called;

       const limit_order_object& limit_order = *limit_itr;

       price match_price  = limit_order.sell_price;
       // There was a check `match_price.validate();` here, which is removed now because it always passes
       price call_pays_price = match_price * bitasset.current_feed.margin_call_pays_ratio(
                                                bitasset.options.extensions.value.margin_call_fee_ratio);
       // Since BSIP74, the call "pays" a bit more collateral per debt than the match price, with the
       // excess being kept by the asset issuer as a margin call fee. In what follows, we use
       // call_pays_price for the black swan check, and for the TCR, but we still use the match_price,
       // of course, to determine what the limit order receives.  Note margin_call_pays_ratio() returns
       // 1/1 if margin_call_fee_ratio is unset (i.e. before BSIP74), so hardfork check is implicit.

       margin_called = true;

       // Although we checked for black swan above, we do one more check to ensure the call order can
       // pay the amount of collateral which we intend to take from it (including margin call fee).  I
       // guess this is just a sanity check, as, I'm not sure how we'd get here without it being
       // detected in the prior swan check, aside perhaps for rounding errors.
       auto usd_to_buy = call_order.get_debt();
       if( usd_to_buy * call_pays_price > call_order.get_collateral() )
       {
          elog( "black swan detected on asset ${symbol} (${id}) at block ${b}",
                ("id",mia.id)("symbol",mia.symbol)("b",head_num) );
          edump((enable_black_swan));
          FC_ASSERT( enable_black_swan );
          globally_settle_asset(mia, bitasset.current_feed.settlement_price );
          return true;
       }

       usd_to_buy.amount = call_order.get_max_debt_to_cover( call_pays_price,
                                                             bitasset.current_feed.settlement_price,
                                                             bitasset.current_feed.maintenance_collateral_ratio,
                                                             bitasset.current_maintenance_collateralization );

       asset usd_for_sale = limit_order.amount_for_sale();
       asset call_pays, call_receives, limit_pays, limit_receives;
       if( usd_to_buy > usd_for_sale )
       {  // fill order
          limit_receives  = usd_for_sale * match_price; // round down, in favor of call order
          call_pays       = usd_for_sale * call_pays_price; // (same as match_price until BSIP-74)

          // Be here, the limit order won't be paying something for nothing, since if it would, it would have
          //   been cancelled elsewhere already (a maker limit order won't be paying something for nothing):
          // * the limit order will be always a maker if entered this function;

          // The remaining amount in the limit order would be too small,
          //   so we should cull the order in fill_limit_order() below.
          // The order would receive 0 even at `match_price`, so it would receive 0 at its own price,
          //   so calling maybe_cull_small() will always cull it.
          call_receives = limit_receives.multiply_and_round_up( match_price );

       } else { // fill call
          call_receives  = usd_to_buy;

          limit_receives = usd_to_buy.multiply_and_round_up( match_price ); // round up, in favor of limit order
          call_pays      = usd_to_buy.multiply_and_round_up( call_pays_price ); // BSIP74; excess is fee.
                                          // Note: TODO: Due to different rounding, couldn't this potentialy be
                                          // one satoshi more than the blackswan check above? Can this bite us?
       }
       limit_pays = call_receives;

       // BSIP74: Margin call fee
       FC_ASSERT(call_pays >= limit_receives);
       const asset margin_call_fee = call_pays - limit_receives;

       // when for_new_limit_order is true, the call order is maker, otherwise the call order is taker
       fill_call_order( call_order, call_pays, call_receives, match_price, for_new_limit_order, margin_call_fee);

       call_collateral_itr = call_collateral_index.lower_bound( call_min );

       auto next_limit_itr = std::next( limit_itr );
       // when for_new_limit_order is true, the limit order is taker, otherwise the limit order is maker
       bool really_filled = fill_limit_order( limit_order, limit_pays, limit_receives, true,
                                              match_price, !for_new_limit_order );
       if( really_filled )
          limit_itr = next_limit_itr;

    } // while call_itr != call_end

    return margin_called;
} FC_CAPTURE_AND_RETHROW() }

void database::pay_order( const account_object& receiver, const asset& receives, const asset& pays )
{
   const auto& balances = receiver.statistics(*this);
   modify( balances, [&]( account_statistics_object& b ){
         if( pays.asset_id == asset_id_type() )
         {
            b.total_core_in_orders -= pays.amount;
         }
   });
   adjust_balance(receiver.get_id(), receives);
}

asset database::calculate_market_fee( const asset_object& trade_asset, const asset& trade_amount,
                                      const bool& is_maker )const
{
   assert( trade_asset.id == trade_amount.asset_id );

   if( !trade_asset.charges_market_fees() )
      return trade_asset.amount(0);
   // Optimization: The fee is zero if the order is a maker, and the maker fee percent is 0%
   if( is_maker && trade_asset.options.market_fee_percent == 0 )
      return trade_asset.amount(0);

   // Optimization: The fee is zero if the order is a taker, and the taker fee percent is 0%
   const optional<uint16_t>& taker_fee_percent = trade_asset.options.extensions.value.taker_fee_percent;
   if(!is_maker && taker_fee_percent.valid() && *taker_fee_percent == 0)
      return trade_asset.amount(0);

   uint16_t fee_percent;
   if (is_maker) {
      // Maker orders are charged the maker fee percent
      fee_percent = trade_asset.options.market_fee_percent;
   } else {
      // Taker orders are charged the taker fee percent if they are valid.  Otherwise, the maker fee percent.
      fee_percent = taker_fee_percent.valid() ? *taker_fee_percent : trade_asset.options.market_fee_percent;
   }

   auto value = detail::calculate_percent(trade_amount.amount, fee_percent);
   asset percent_fee = trade_asset.amount(value);

   if( percent_fee.amount > trade_asset.options.max_market_fee )
      percent_fee.amount = trade_asset.options.max_market_fee;

   return percent_fee;
}


asset database::pay_market_fees(const account_object* seller, const asset_object& recv_asset, const asset& receives,
                                const bool& is_maker, const optional<asset>& calculated_market_fees )
{
   const auto market_fees = ( calculated_market_fees.valid() ? *calculated_market_fees
                                    : calculate_market_fee( recv_asset, receives, is_maker ) );
   auto issuer_fees = market_fees;
   FC_ASSERT( issuer_fees <= receives, "Market fee shouldn't be greater than receives");
   //Don't dirty undo state if not actually collecting any fees
   if ( issuer_fees.amount > 0 )
   {
      // Share market fees to the network
      const uint16_t network_percent = get_global_properties().parameters.get_market_fee_network_percent();
      if( network_percent > 0 )
      {
         const auto network_fees_amt = detail::calculate_percent( issuer_fees.amount, network_percent );
         FC_ASSERT( network_fees_amt <= issuer_fees.amount,
                    "Fee shared to the network shouldn't be greater than total market fee" );
         if( network_fees_amt > 0 )
         {
            const asset network_fees = recv_asset.amount( network_fees_amt );
            deposit_market_fee_vesting_balance( GRAPHENE_COMMITTEE_ACCOUNT, network_fees );
            issuer_fees -= network_fees;
         }
      }
   }

   // Process the remaining fees
   if ( issuer_fees.amount > 0 )
   {
      // calculate and pay rewards
      asset reward = recv_asset.amount(0);

      auto is_rewards_allowed = [&recv_asset, seller]() {
         if (seller == nullptr)
            return false;
         const auto &white_list = recv_asset.options.extensions.value.whitelist_market_fee_sharing;
         return ( !white_list || (*white_list).empty() 
               || ( (*white_list).find(seller->registrar) != (*white_list).end() ) );
      };

      if ( is_rewards_allowed() )
      {
         const auto reward_percent = recv_asset.options.extensions.value.reward_percent;
         if ( reward_percent && *reward_percent )
         {
            const auto reward_value = detail::calculate_percent(issuer_fees.amount, *reward_percent);
            if ( reward_value > 0 && is_authorized_asset(*this, seller->registrar(*this), recv_asset) )
            {
               reward = recv_asset.amount(reward_value);
               FC_ASSERT( reward <= issuer_fees, "Market reward should not be greater than issuer fees");
               // cut referrer percent from reward
               auto registrar_reward = reward;

               auto registrar = seller->registrar;
               auto referrer = seller->referrer;

               // for funds going to temp-account, redirect to committee-account
               if( registrar == GRAPHENE_TEMP_ACCOUNT )
                  registrar = GRAPHENE_COMMITTEE_ACCOUNT;
               if( referrer == GRAPHENE_TEMP_ACCOUNT )
                  referrer = GRAPHENE_COMMITTEE_ACCOUNT;

               if( referrer != registrar )
               {
                  const auto referrer_rewards_value = detail::calculate_percent( reward.amount,
                                                                                 seller->referrer_rewards_percentage );

                  if ( referrer_rewards_value > 0 && is_authorized_asset(*this, referrer(*this), recv_asset) )
                  {
                     FC_ASSERT ( referrer_rewards_value <= reward.amount.value,
                                 "Referrer reward shouldn't be greater than total reward" );
                     const asset referrer_reward = recv_asset.amount(referrer_rewards_value);
                     registrar_reward -= referrer_reward;
                     deposit_market_fee_vesting_balance(referrer, referrer_reward);
                  }
               }
               if( registrar_reward.amount > 0 )
                  deposit_market_fee_vesting_balance(registrar, registrar_reward);
            }
         }
      }

      if( issuer_fees.amount > reward.amount )
      {
         const auto& recv_dyn_data = recv_asset.dynamic_asset_data_id(*this);
         modify( recv_dyn_data, [&issuer_fees, &reward]( asset_dynamic_data_object& obj ){
            obj.accumulated_fees += issuer_fees.amount - reward.amount;
         });
      }
   }

   return market_fees;
}

/***
 * @brief Calculate force-settlement fee and give it to issuer of the settled asset
 * @param collecting_asset the smart asset object which should receive the fee
 * @param collat_receives the amount of collateral the settler would expect to receive absent this fee
 *     (fee is computed as a percentage of this amount)
 * @return asset denoting the amount of fee collected
 */
asset database::pay_force_settle_fees(const asset_object& collecting_asset, const asset& collat_receives)
{
   FC_ASSERT( collecting_asset.get_id() != collat_receives.asset_id );

   const bitasset_options& collecting_bitasset_opts = collecting_asset.bitasset_data(*this).options;

   if( !collecting_bitasset_opts.extensions.value.force_settle_fee_percent.valid()
         || *collecting_bitasset_opts.extensions.value.force_settle_fee_percent == 0 )
      return asset{ 0, collat_receives.asset_id };

   auto value = detail::calculate_percent(collat_receives.amount,
                                          *collecting_bitasset_opts.extensions.value.force_settle_fee_percent);
   asset settle_fee = asset{ value, collat_receives.asset_id };

   // Deposit fee in asset's dynamic data object:
   if( value > 0) {
      collecting_asset.accumulate_fee(*this, settle_fee);
   }
   return settle_fee;
}

} }
