/**
 * The Revolution Populi Project
 * Copyright (C) 2022 Revolution Populi Limited
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
 */

#pragma once

#include <graphene/protocol/base.hpp>
#include <graphene/protocol/asset.hpp>
#include <graphene/protocol/authority.hpp>
#include <graphene/protocol/buyback.hpp>
#include <graphene/protocol/special_authority.hpp>
#include <graphene/protocol/vote.hpp>

namespace graphene { namespace protocol {

   /**
    * @brief Create a content card object
    *
    * This operation is used to create the content_card_v2_object.
    */
   struct content_card_v2_create_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee       = 20 * GRAPHENE_BLOCKCHAIN_PRECISION;
         uint32_t price_per_kbyte = 10 * GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset           fee;

      account_id_type subject_account;
      string          hash;
      string          url;
      string          type;
      string          description;
      string          content_key;
      string          storage_data;

      account_id_type fee_payer()const { return subject_account; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         // owner_account should be required anyway as it is the fee_payer(), but we insert it here just to be sure
         a.insert( subject_account );
      }
   };

   /**
    * @brief Update a content card object
    *
    * This operation is used to update the content_card_v2_object.
    */
   struct content_card_v2_update_operation : public base_operation
   {
      struct fee_parameters_type {
         uint64_t fee       = 20 * GRAPHENE_BLOCKCHAIN_PRECISION;
         uint32_t price_per_kbyte = 10 * GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset           fee;

      account_id_type subject_account;
      string          hash;
      string          url;
      string          type;
      string          description;
      string          content_key;
      string          storage_data;

      account_id_type fee_payer()const { return subject_account; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         // owner_account should be required anyway as it is the fee_payer(), but we insert it here just to be sure
         a.insert( subject_account );
      }
   };

   /**
    * @brief Remove a content card object
    *
    * This operation is used to remove the content_card_v2_object.
    */
   struct content_card_v2_remove_operation : public base_operation
   {
      struct fee_parameters_type { uint64_t fee = GRAPHENE_BLOCKCHAIN_PRECISION; };

      asset           fee; // always zero

      account_id_type subject_account;
      content_card_v2_id_type content_id;

      account_id_type fee_payer()const { return subject_account; }
      void            validate()const;
      share_type      calculate_fee(const fee_parameters_type& )const;

      void get_required_active_authorities( flat_set<account_id_type>& a )const
      {
         // owner_account should be required anyway as it is the fee_payer(), but we insert it here just to be sure
         a.insert( subject_account );
      }
   };


} } // graphene::protocol

FC_REFLECT( graphene::protocol::content_card_v2_create_operation::fee_parameters_type, (fee)(price_per_kbyte) )
FC_REFLECT( graphene::protocol::content_card_v2_create_operation,
            (fee)
            (subject_account)(hash)(url)(type)(description)(content_key)(storage_data)
          )
FC_REFLECT( graphene::protocol::content_card_v2_update_operation::fee_parameters_type, (fee)(price_per_kbyte) )
FC_REFLECT( graphene::protocol::content_card_v2_update_operation,
            (fee)
            (subject_account)(hash)(url)(type)(description)(content_key)(storage_data)
          )
FC_REFLECT( graphene::protocol::content_card_v2_remove_operation::fee_parameters_type, (fee) )
FC_REFLECT( graphene::protocol::content_card_v2_remove_operation,
            (fee)
            (subject_account)(content_id)
          )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::content_card_v2_create_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::content_card_v2_update_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::content_card_v2_remove_operation )
