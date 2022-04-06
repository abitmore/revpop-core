/*
 * Copyright (c) 2018 Bitshares Foundation, and contributors.
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

#include <vector>
#include <boost/test/unit_test.hpp>


#include <graphene/chain/database.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/balance_object.hpp>
#include <graphene/chain/budget_record_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/withdraw_permission_object.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/worker_object.hpp>
#include <graphene/chain/asset_evaluator.hpp>

#include <graphene/utilities/tempdir.hpp>

#include <fc/crypto/digest.hpp>
#include <fc/log/log_message.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_FIXTURE_TEST_SUITE( bitasset_tests, database_fixture )

/*****
 * @brief helper method to change a backing asset to a new one
 * @param fixture the database_fixture
 * @param signing_key the signer
 * @param asset_id_to_update asset to update
 * @param new_backing_asset_id the new backing asset
 */
void change_backing_asset(database_fixture& fixture, const fc::ecc::private_key& signing_key,
      asset_id_type asset_id_to_update, asset_id_type new_backing_asset_id)
{
   try
   {
      asset_update_bitasset_operation ba_op;
      const asset_object& asset_to_update = asset_id_to_update(fixture.db);
      ba_op.asset_to_update = asset_id_to_update;
      ba_op.issuer = asset_to_update.issuer;
      ba_op.new_options.short_backing_asset = new_backing_asset_id;
      fixture.trx.operations.push_back(ba_op);
      fixture.sign(fixture.trx, signing_key);
      PUSH_TX(fixture.db, fixture.trx, ~0);
      fixture.generate_block();
      fixture.trx.clear();
   }
   catch (fc::exception& ex)
   {
      BOOST_FAIL( "Exception thrown in change_backing_asset. Exception was: " +
            ex.to_string(fc::log_level(fc::log_level::all)) );
   }
}

/******
 * @brief helper method to turn witness_fed_asset on and off
 * @param fixture the database_fixture
 * @param new_issuer optionally change the issuer
 * @param signing_key signer
 * @param asset_id asset we want to change
 * @param witness_fed true if you want this to be a witness fed asset
 */
void change_asset_options(database_fixture& fixture, const optional<account_id_type>& new_issuer,
      const fc::ecc::private_key& signing_key,
      asset_id_type asset_id, bool witness_fed)
{
   asset_update_operation op;
   const asset_object& obj = asset_id(fixture.db);
   op.asset_to_update = asset_id;
   op.issuer = obj.issuer;
   op.new_options = obj.options;
   if (witness_fed)
   {
      op.new_options.flags |= witness_fed_asset;
      op.new_options.flags &= ~committee_fed_asset;
   }
   else
   {
      op.new_options.flags &= ~witness_fed_asset; // we don't care about the committee flag here
   }
   fixture.trx.operations.push_back(op);
   fixture.sign( fixture.trx, signing_key );
   PUSH_TX( fixture.db, fixture.trx, ~0 );
   if (new_issuer)
   {
      asset_update_issuer_operation upd_op;
      const asset_object& obj = asset_id(fixture.db);
      upd_op.asset_to_update = asset_id;
      upd_op.issuer = obj.issuer;
      upd_op.new_issuer = *new_issuer;
      fixture.trx.operations.push_back(upd_op);
      fixture.sign( fixture.trx, signing_key );
      PUSH_TX( fixture.db, fixture.trx, ~0 );
   }
   fixture.generate_block();
   fixture.trx.clear();

}

/*********
 * @brief helper method to create a coin backed by a bitasset
 * @param fixture the database_fixture
 * @param index added to name of the coin
 * @param backing the backing asset
 * @param signing_key the signing key
 */
const graphene::chain::asset_object& create_bitasset_backed(graphene::chain::database_fixture& fixture,
      int index, graphene::chain::asset_id_type backing, const fc::ecc::private_key& signing_key)
{
   // create the coin
   std::string name = "COIN" + std::to_string(index + 1) + "TEST";
   const graphene::chain::asset_object& obj = fixture.create_bitasset(name);
   asset_id_type asset_id = obj.get_id();
   // adjust the backing asset
   change_backing_asset(fixture, signing_key, asset_id, backing);
   fixture.trx.set_expiration(fixture.db.get_dynamic_global_properties().next_maintenance_time);
   return obj;
}


class bitasset_evaluator_wrapper : public asset_update_bitasset_evaluator
{
public:
   void set_db(database& db)
   {
      this->trx_state = new transaction_evaluation_state(&db);
   }
};

struct assets_922_931
{
   asset_id_type bit_usd;
   asset_id_type bit_usdbacked;
   asset_id_type bit_usdbacked2;
   asset_id_type bit_child_bitasset;
   asset_id_type bit_parent;
   asset_id_type user_issued;
   asset_id_type six_precision;
   asset_id_type prediction;
};

assets_922_931 create_assets_922_931(database_fixture* fixture)
{
   assets_922_931 asset_objs;
   BOOST_TEST_MESSAGE( "Create USDBIT" );
   asset_objs.bit_usd = fixture->create_bitasset( "USDBIT", GRAPHENE_COMMITTEE_ACCOUNT ).get_id();

   BOOST_TEST_MESSAGE( "Create USDBACKED" );
   asset_objs.bit_usdbacked = fixture->create_bitasset( "USDBACKED", GRAPHENE_COMMITTEE_ACCOUNT,
         100, charge_market_fee, 2, asset_objs.bit_usd ).get_id();

   BOOST_TEST_MESSAGE( "Create USDBACKEDII" );
   asset_objs.bit_usdbacked2 = fixture->create_bitasset( "USDBACKEDII", GRAPHENE_WITNESS_ACCOUNT,
         100, charge_market_fee, 2, asset_objs.bit_usd ).get_id();

   BOOST_TEST_MESSAGE( "Create PARENT" );
   asset_objs.bit_parent = fixture->create_bitasset( "PARENT", GRAPHENE_WITNESS_ACCOUNT).get_id();

   BOOST_TEST_MESSAGE( "Create CHILDUSER" );
   asset_objs.bit_child_bitasset = fixture->create_bitasset( "CHILDUSER", GRAPHENE_WITNESS_ACCOUNT,
         100, charge_market_fee, 2, asset_objs.bit_parent ).get_id();

   BOOST_TEST_MESSAGE( "Create user issued USERISSUED" );
   asset_objs.user_issued = fixture->create_user_issued_asset( "USERISSUED",
         GRAPHENE_WITNESS_ACCOUNT(fixture->db), charge_market_fee ).get_id();

   BOOST_TEST_MESSAGE( "Create a user-issued asset with a precision of 6" );
   asset_objs.six_precision = fixture->create_user_issued_asset( "SIXPRECISION", GRAPHENE_WITNESS_ACCOUNT(fixture->db),
         charge_market_fee, price(asset(1, asset_id_type(1)), asset(1)), 6 ).get_id();

   BOOST_TEST_MESSAGE( "Create Prediction market with precision of 6, backed by SIXPRECISION" );
   asset_objs.prediction = fixture->create_prediction_market( "PREDICTION", GRAPHENE_WITNESS_ACCOUNT,
         100, charge_market_fee, 6, asset_objs.six_precision ).get_id();

   return asset_objs;
}
/******
 * @brief Test various bitasset asserts within the asset_evaluator
 */
BOOST_AUTO_TEST_CASE( bitasset_evaluator_test_after_922_931 )
{
   auto global_params = db.get_global_properties().parameters;
   generate_blocks( global_params.maintenance_interval );
   trx.set_expiration( db.head_block_time() + fc::seconds( global_params.maximum_time_until_expiration ));

   ACTORS( (nathan) (john) );

   assets_922_931 asset_objs = create_assets_922_931( this );
   const asset_id_type& bit_usd_id = asset_objs.bit_usd;

   // make a generic operation
   bitasset_evaluator_wrapper evaluator;
   evaluator.set_db( db );
   asset_update_bitasset_operation op;
   op.asset_to_update = bit_usd_id;
   op.issuer = asset_objs.bit_usd(db).issuer;
   op.new_options = asset_objs.bit_usd(db).bitasset_data(db).options;

   // this should pass
   BOOST_TEST_MESSAGE( "Evaluating a good operation" );
   BOOST_CHECK( evaluator.evaluate(op).is_type<void_result>() );

   // test with a market issued asset
   BOOST_TEST_MESSAGE( "Sending a non-bitasset." );
   op.asset_to_update = asset_objs.user_issued;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Cannot update BitAsset-specific settings on a non-BitAsset" );
   op.asset_to_update = bit_usd_id;

   // test changing issuer
   BOOST_TEST_MESSAGE( "Test changing issuer." );
   account_id_type original_issuer = op.issuer;
   op.issuer = john_id;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Only asset issuer can update" );
   op.issuer = original_issuer;

   // bad backing_asset
   BOOST_TEST_MESSAGE( "Non-existent backing asset." );
   asset_id_type correct_asset_id = op.new_options.short_backing_asset;
   op.new_options.short_backing_asset = asset_id_type();
   op.new_options.short_backing_asset.instance = 123;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Unable to find" );
   op.new_options.short_backing_asset = correct_asset_id;

   // now check the things that are wrong and won't pass
   BOOST_TEST_MESSAGE( "Now check the things that are wrong and won't pass" );

   // back by self
   BOOST_TEST_MESSAGE( "Back by itself" );
   op.new_options.short_backing_asset = bit_usd_id;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Cannot update an asset to be backed by itself" );
   op.new_options.short_backing_asset = correct_asset_id;

   // prediction market with different precision
   BOOST_TEST_MESSAGE( "Prediction market with different precision" );
   op.asset_to_update = asset_objs.prediction;
   op.issuer = asset_objs.prediction(db).issuer;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "The precision of the asset and backing asset must" );
   op.asset_to_update = bit_usd_id;
   op.issuer = asset_objs.bit_usd(db).issuer;

   // checking old backing asset instead of new backing asset
   BOOST_TEST_MESSAGE( "Correctly checking new backing asset rather than old backing asset" );
   op.new_options.short_backing_asset = asset_objs.six_precision;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "which is not market issued asset nor CORE." );
   op.new_options.short_backing_asset = asset_objs.prediction;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "which is not backed by CORE" );
   op.new_options.short_backing_asset = correct_asset_id;

   // CHILD is a non-committee asset backed by PARENT which is backed by CORE
   // Cannot change PARENT's backing asset from CORE to something that is not [CORE | UIA]
   // because that will make CHILD be backed by an asset that is not itself backed by CORE or a UIA.
   BOOST_TEST_MESSAGE( "Attempting to change PARENT to be backed by a non-core and non-user-issued asset" );
   op.asset_to_update = asset_objs.bit_parent;
   op.issuer = asset_objs.bit_parent(db).issuer;
   op.new_options.short_backing_asset = asset_objs.bit_usdbacked;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "A non-blockchain controlled BitAsset would be invalidated" );
   // changing the backing asset to a UIA should work
   BOOST_TEST_MESSAGE( "Switching to a backing asset that is a UIA should work." );
   op.new_options.short_backing_asset = asset_objs.user_issued;
   BOOST_CHECK( evaluator.evaluate(op).is_type<void_result>() );
   // A -> B -> C, change B to be backed by A (circular backing)
   BOOST_TEST_MESSAGE( "Check for circular backing. This should generate an exception" );
   op.new_options.short_backing_asset = asset_objs.bit_child_bitasset;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "'A' backed by 'B' backed by 'A'" );
   op.new_options.short_backing_asset = asset_objs.user_issued;
   BOOST_CHECK( evaluator.evaluate(op).is_type<void_result>() );
   BOOST_TEST_MESSAGE( "Creating CHILDCOMMITTEE" );
   // CHILDCOMMITTEE is a committee asset backed by PARENT which is backed by CORE
   // Cannot change PARENT's backing asset from CORE to something else because that will make CHILDCOMMITTEE
   // be backed by an asset that is not itself backed by CORE
   create_bitasset( "CHILDCOMMITTEE", GRAPHENE_COMMITTEE_ACCOUNT, 100, charge_market_fee, 2,
         asset_objs.bit_parent );
   // it should again not work
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "A blockchain-controlled market asset would be invalidated" );
   op.asset_to_update = asset_objs.bit_usd;
   op.issuer = asset_objs.bit_usd(db).issuer;
   op.new_options.short_backing_asset = correct_asset_id;

   // USDBACKED is backed by USDBIT (which is backed by CORE)
   // USDBACKEDII is backed by USDBIT
   // We should not be able to make USDBACKEDII be backed by USDBACKED
   // because that would be a MPA backed by MPA backed by MPA.
   BOOST_TEST_MESSAGE( "MPA -> MPA -> MPA not allowed" );
   op.asset_to_update = asset_objs.bit_usdbacked2;
   op.issuer = asset_objs.bit_usdbacked2(db).issuer;
   op.new_options.short_backing_asset = asset_objs.bit_usdbacked;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op),
         "A BitAsset cannot be backed by a BitAsset that itself is backed by a BitAsset" );
   // set everything to a more normal state
   op.asset_to_update = asset_objs.bit_usdbacked;
   op.issuer = asset_objs.bit_usd(db).issuer;
   op.new_options.short_backing_asset = asset_id_type();

   // Feed lifetime must exceed block interval
   BOOST_TEST_MESSAGE( "Feed lifetime less than or equal to block interval" );
   const auto good_feed_lifetime = op.new_options.feed_lifetime_sec;
   op.new_options.feed_lifetime_sec = db.get_global_properties().parameters.block_interval;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Feed lifetime must exceed block" );
   op.new_options.feed_lifetime_sec = db.get_global_properties().parameters.block_interval - 1; // default interval > 1
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Feed lifetime must exceed block" );
   op.new_options.feed_lifetime_sec = good_feed_lifetime;

   // Force settlement delay must exceed block interval.
   BOOST_TEST_MESSAGE( "Force settlement delay less than or equal to block interval" );
   const auto good_force_settlement_delay_sec = op.new_options.force_settlement_delay_sec;
   op.new_options.force_settlement_delay_sec = db.get_global_properties().parameters.block_interval;
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Force settlement delay must" );
   op.new_options.force_settlement_delay_sec = db.get_global_properties().parameters.block_interval - 1; // default interval > 1
   REQUIRE_EXCEPTION_WITH_TEXT( evaluator.evaluate(op), "Force settlement delay must" );
   op.new_options.force_settlement_delay_sec = good_force_settlement_delay_sec;

   // this should pass
   BOOST_TEST_MESSAGE( "We should be all good again." );
   BOOST_CHECK( evaluator.evaluate(op).is_type<void_result>() );

}

BOOST_AUTO_TEST_CASE( bitasset_secondary_index )
{
   ACTORS( (nathan) );

   graphene::chain::asset_id_type core_id;
   BOOST_TEST_MESSAGE( "Running test bitasset_secondary_index" );
   BOOST_TEST_MESSAGE( "Core asset id: " + fc::json::to_pretty_string( core_id ) );
   BOOST_TEST_MESSAGE("Create coins");
   try
   {
      // make 5 coins (backed by core)
      for(int i = 0; i < 5; i++)
      {
         create_bitasset_backed(*this, i, core_id, nathan_private_key);
      }
      // make the next 5 (10-14) be backed by COIN1
      graphene::chain::asset_id_type coin1_id = get_asset("COIN1TEST").get_id();
      for(int i = 5; i < 10; i++)
      {
         create_bitasset_backed(*this, i, coin1_id, nathan_private_key);
      }
      // make the next 5 (15-19) be backed by COIN2
      graphene::chain::asset_id_type coin2_id = get_asset("COIN2TEST").get_id();
      for(int i = 10; i < 15; i++)
      {
         create_bitasset_backed(*this, i, coin2_id, nathan_private_key);
      }
      // make the last 5 be backed by core
      for(int i = 15; i < 20; i++)
      {
         create_bitasset_backed(*this, i, core_id, nathan_private_key);
      }

      BOOST_TEST_MESSAGE("Searching for all coins backed by CORE");
      const auto& idx = db.get_index_type<graphene::chain::asset_bitasset_data_index>().indices().get<graphene::chain::by_short_backing_asset>();
      auto core_itr = idx.equal_range( core_id );
      BOOST_TEST_MESSAGE("Searching for all coins backed by COIN1");
      auto coin1_itr = idx.equal_range( coin1_id );
      BOOST_TEST_MESSAGE("Searching for all coins backed by COIN2");
      auto coin2_itr = idx.equal_range( coin2_id );

      int core_count = 0, coin1_count = 0, coin2_count = 0;

      BOOST_TEST_MESSAGE("Counting coins in each category");

      for( auto itr = core_itr.first ; itr != core_itr.second; ++itr)
      {
         BOOST_CHECK(itr->options.short_backing_asset == core_id);
         BOOST_TEST_MESSAGE( fc::json::to_pretty_string(itr->asset_id) + " is backed by CORE" );
         core_count++;
      }
      for( auto itr = coin1_itr.first ; itr != coin1_itr.second; ++itr )
      {
         BOOST_CHECK(itr->options.short_backing_asset == coin1_id);
         BOOST_TEST_MESSAGE( fc::json::to_pretty_string( itr->asset_id) + " is backed by COIN1TEST" );
         coin1_count++;
      }
      for( auto itr = coin2_itr.first; itr != coin2_itr.second; ++itr )
      {
         BOOST_CHECK(itr->options.short_backing_asset == coin2_id);
         BOOST_TEST_MESSAGE( fc::json::to_pretty_string( itr->asset_id) + " is backed by COIN2TEST" );
         coin2_count++;
      }

      BOOST_CHECK( core_count >= 10 );
      BOOST_CHECK_EQUAL( coin1_count, 5 );
      BOOST_CHECK_EQUAL( coin2_count, 5 );
   }
   catch (fc::exception& ex)
   {
      BOOST_FAIL(ex.to_string(fc::log_level(fc::log_level::all)));
   }
}


   /**
    * Test the claiming of collateral asset fees before HARDFORK_CORE_BSIP_87_74_COLLATFEE_TIME.
    *
    * Test prohibitions against changing of the backing/collateral asset for a smart asset
    * if any collateral asset fees are available to be claimed.
    */
   BOOST_AUTO_TEST_CASE(change_backing_asset_prohibitions) {
      try {
         /**
          * Initialize
          */
         // Initialize for the current time
         trx.clear();
         set_expiration(db, trx);

         // Initialize actors
         ACTORS((smartissuer)(feedproducer)); // Actors for smart asset
         ACTORS((jill)(izzy)); // Actors for user-issued assets
         ACTORS((alice)); // Actors who hold balances

         price price(asset(1, asset_id_type(1)), asset(1));
         uint16_t market_fee_percent = 20 * GRAPHENE_1_PERCENT;
         create_user_issued_asset("JCOIN", jill, charge_market_fee, price, 2, market_fee_percent);
         generate_block(); trx.clear(); set_expiration(db, trx);
         const asset_object jillcoin = get_asset("JCOIN");
         const int64_t jillcoin_unit = 100; // 100 satoshi JILLCOIN in 1 JILLCOIN

         create_user_issued_asset("ICOIN", izzy_id(db), charge_market_fee, price, 2, market_fee_percent);
         generate_block();
         const asset_object izzycoin = get_asset("ICOIN");

         // Create the smart asset backed by JCOIN
         const uint16_t smartbit_market_fee_percent = 2 * GRAPHENE_1_PERCENT;
         create_bitasset("SMARTBIT", smartissuer_id, smartbit_market_fee_percent,
                         charge_market_fee, 2, jillcoin.id);

         // Obtain asset object after a block is generated to obtain the final object that is commited to the database
         generate_block(); trx.clear(); set_expiration(db, trx);
         const asset_object &smartbit = get_asset("SMARTBIT");
         const asset_bitasset_data_object& smartbit_bitasset_data = (*smartbit.bitasset_data_id)(db);
         // Confirm that the asset is to be backed by JCOIN
         BOOST_CHECK(smartbit_bitasset_data.options.short_backing_asset == jillcoin.id);

         // Fund balances of the actors
         issue_uia(alice, jillcoin.amount(5000 * jillcoin_unit));
         BOOST_REQUIRE_EQUAL(get_balance(alice, jillcoin), 5000 * jillcoin_unit);
         BOOST_REQUIRE_EQUAL(get_balance(alice, smartbit), 0);


         /**
          * Claim any amount of collateral asset fees.
          */
         trx.clear();
         asset_claim_fees_operation claim_op;
         claim_op.issuer = smartissuer_id;
         claim_op.extensions.value.claim_from_asset_id = smartbit.id;
         claim_op.amount_to_claim = jillcoin.amount(5 * jillcoin_unit);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         REQUIRE_EXCEPTION_WITH_TEXT(PUSH_TX(db, trx), "Attempt to claim more backing-asset fees than have accumulated within asset SMARTBIT");


         /**
          * Propose to claim any amount of collateral asset fees.
          */
         proposal_create_operation cop;
         cop.review_period_seconds = 86400;
         uint32_t buffer_seconds = 60 * 60;
         cop.expiration_time = db.head_block_time() + *cop.review_period_seconds + buffer_seconds;
         cop.fee_paying_account = GRAPHENE_TEMP_ACCOUNT;
         cop.proposed_ops.emplace_back(claim_op);

         trx.clear();
         trx.operations.push_back(cop);
         // sign(trx, smartissuer_private_key);
         PUSH_TX( db, trx );


         /**
          * Advance to when the collateral fee container is activated
          */
         generate_block(); trx.clear(); set_expiration(db, trx);


         /**
          * Cause some collateral of JCOIN to be accumulated as collateral fee within the SMARTBIT asset type
          */
         // HACK: Before BSIP74 or BSIP87 are introduced, it is not formally possible to accumulate collateral fees.
         // Therefore, the accumulation for this test will be informally induced by direct manipulation of the database.
         // More formal tests will be provided with the PR for either BSIP74 or BSIP87.
         // IMPORTANT: The use of this hack requires that no additional blocks are subsequently generated!
         asset accumulation_amount = jillcoin.amount(40 * jillcoin_unit); // JCOIN
         db.adjust_balance(alice_id, -accumulation_amount); // Deduct 40 JCOIN from alice as a "collateral fee"
         smartbit.accumulate_fee(db, accumulation_amount); // Add 40 JCOIN from alice as a "collateral fee"
         BOOST_REQUIRE_EQUAL(get_balance(alice, jillcoin), (5000 * jillcoin_unit) - (40 * jillcoin_unit));
         BOOST_CHECK(smartbit.dynamic_asset_data_id(db).accumulated_collateral_fees == accumulation_amount.amount);


         /**
          * Attempt to change the backing asset.  This should fail because there are unclaimed collateral fees.
          */
         trx.clear();
         asset_update_bitasset_operation change_backing_asset_op;
         change_backing_asset_op.asset_to_update = smartbit.id;
         change_backing_asset_op.issuer = smartissuer_id;
         change_backing_asset_op.new_options.short_backing_asset = izzycoin.id;
         trx.operations.push_back(change_backing_asset_op);
         sign(trx, smartissuer_private_key);
         REQUIRE_EXCEPTION_WITH_TEXT(PUSH_TX(db, trx), "Must claim collateral-denominated fees");


         /**
          * Attempt to claim a negative amount of the collateral asset fees.
          * This should fail because positive amounts are required.
          */
         trx.clear();
         claim_op.amount_to_claim = jillcoin.amount(-9 * jillcoin_unit);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         REQUIRE_EXCEPTION_WITH_TEXT(PUSH_TX(db, trx), "amount_to_claim.amount > 0");


         /**
          * Attempt to claim 0 amount of the collateral asset fees.
          * This should fail because positive amounts are required.
          */
         trx.clear();
         claim_op.amount_to_claim = jillcoin.amount(0 * jillcoin_unit);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         REQUIRE_EXCEPTION_WITH_TEXT(PUSH_TX(db, trx), "amount_to_claim.amount > 0");


         /**
          * Attempt to claim excessive amount of collateral asset fees.
          * This should fail because there are insufficient collateral fees.
          */
         trx.clear();
         claim_op.amount_to_claim = accumulation_amount + jillcoin.amount(1);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         REQUIRE_EXCEPTION_WITH_TEXT(PUSH_TX(db, trx), "Attempt to claim more backing-asset fees");


         /**
          * Claim some of the collateral asset fees
          */
         share_type part_of_accumulated_fees = accumulation_amount.amount / 4;
         FC_ASSERT(part_of_accumulated_fees.value > 0); // Partial claim should be positive
         share_type remainder_accumulated_fees = accumulation_amount.amount - part_of_accumulated_fees;
         FC_ASSERT(remainder_accumulated_fees.value > 0); // Planned remainder should be positive
         trx.clear();
         claim_op.amount_to_claim = jillcoin.amount(part_of_accumulated_fees);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         PUSH_TX(db, trx);
         BOOST_CHECK(smartbit.dynamic_asset_data_id(db).accumulated_collateral_fees == remainder_accumulated_fees);


         /**
          * Claim all the remaining collateral asset fees
          */
         trx.clear();
         claim_op.amount_to_claim = jillcoin.amount(remainder_accumulated_fees);
         trx.operations.push_back(claim_op);
         sign(trx, smartissuer_private_key);
         PUSH_TX(db, trx);
         BOOST_CHECK(smartbit.dynamic_asset_data_id(db).accumulated_collateral_fees == 0); // 0 remainder


         /**
          * Attempt to change the backing asset.
          * This should succeed because there are no collateral asset fees are waiting to be claimed.
          */
         trx.clear();
         trx.operations.push_back(change_backing_asset_op);
         sign(trx, smartissuer_private_key);
         PUSH_TX(db, trx);

         // Confirm the change to the backing asset
         BOOST_CHECK(smartbit_bitasset_data.options.short_backing_asset == izzycoin.id);

      } FC_LOG_AND_RETHROW()
   }

BOOST_AUTO_TEST_SUITE_END()
