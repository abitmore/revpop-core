/*
 * Copyright (c) 2019 Contributors.
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

#include <graphene/protocol/operations.hpp>

#include <fc/reflect/typelist.hpp>

namespace graphene { namespace protocol {
namespace typelist = fc::typelist;

// To make the build gentler on RAM, break the operation list into several pieces to build over several files
using operation_list_1 = static_variant<typelist::slice<operation::list, 0, 5>>;
using operation_list_2 = static_variant<typelist::slice<operation::list, 5, 9>>;
using operation_list_3 = static_variant<typelist::slice<operation::list, 9, 11>>;
using operation_list_4 = static_variant<typelist::slice<operation::list, 11, 12>>;
using operation_list_5 = static_variant<typelist::slice<operation::list, 12, 15>>;
using operation_list_6 = static_variant<typelist::slice<operation::list, 15, 22>>;
using operation_list_7 = static_variant<typelist::slice<operation::list, 22, 29>>;
using operation_list_8 = static_variant<typelist::slice<operation::list, 29, 32>>;
using operation_list_9 = static_variant<typelist::slice<operation::list, 32, 35>>;
using operation_list_10 = static_variant<typelist::slice<operation::list, 35, 37>>;
using operation_list_11 = static_variant<typelist::builder<>
                                                ::add<asset_claim_fees_operation> // 38
                                                ::add_list<typelist::slice<operation::list, 40, 63>>
                                                ::finalize>;
using operation_list_12 = static_variant<typelist::builder<>
                                                ::add_list<typelist::slice<operation::list, 63, 71>>
                                                ::add<htlc_extend_operation> // 72
                                                ::finalize>;
using operation_list_13 = static_variant<typelist::slice<operation::list, 74, 77>>;
using virtual_operations_list = static_variant<
                                               asset_settle_cancel_operation, // 37
                                               fba_distribute_operation,      // 39
                                               htlc_redeemed_operation,       // 71
                                               htlc_refund_operation,         // 73
                                               fill_order_operation           // 77
                                              >;

object_restriction_predicate<operation> get_restriction_predicate_list_1(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_2(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_3(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_4(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_5(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_6(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_7(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_8(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_9(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_10(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_11(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_12(size_t idx, vector<restriction> rs);
object_restriction_predicate<operation> get_restriction_predicate_list_13(size_t idx, vector<restriction> rs);

} } // namespace graphene::protocol
