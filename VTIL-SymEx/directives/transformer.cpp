// Copyright (c) 2020 Can Boluk and contributors of the VTIL Project   
// All rights reserved.   
//    
// Redistribution and use in source and binary forms, with or without   
// modification, are permitted provided that the following conditions are met: 
//    
// 1. Redistributions of source code must retain the above copyright notice,   
//    this list of conditions and the following disclaimer.   
// 2. Redistributions in binary form must reproduce the above copyright   
//    notice, this list of conditions and the following disclaimer in the   
//    documentation and/or other materials provided with the distribution.   
// 3. Neither the name of mosquitto nor the names of its   
//    contributors may be used to endorse or promote products derived from   
//    this software without specific prior written permission.   
//    
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE   
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR   
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF   
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  
// POSSIBILITY OF SUCH DAMAGE.        
//
#include "transformer.hpp"
#include <vtil/utility>
#include "../simplifier/simplifier.hpp"

namespace vtil::symbolic
{
	using namespace directive;

	// Translates the given directive into an expression (of size given) using the symbol table.
	// - If speculative flag is set, it will either return a dummy reference if the expression could be built,
	//   or a null reference if it would fail.
	//
	expression::reference translate( const symbol_table_t& sym,
									 const instance::reference& dir,
									 bitcnt_t bit_cnt,
									 bool speculative_condition )
	{
		using namespace logger;
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		scope_padding _p( 1 );
		log<CON_BLU>( "[%s].\n", dir->to_string() );
#endif

		// Dummy expression used just to indicate success if speculative_condition is used, if seen in
		// output of the simplifier, there's a major bug.
		//
		static const expression::reference dummy_expression = expression{ { "@dummmy" }, 1 };

		// If expression operator:
		//
		if ( dir->op < math::operator_id::max )
		{
			// If directive is a variable or a constant, translate to expression equivalent.
			//
			if ( dir->op == math::operator_id::invalid )
			{
				if ( !dir->id ) return expression{ dir->get().value(), bit_cnt };
				else            return sym.translate( dir );
			}
			// If it is an expression:
			//
			else
			{
				// If speculative condition parsing no need to build an expression, simplfy
				// check if all operands can also be speculatively built.
				//
				if ( speculative_condition )
				{
					if ( dir->lhs && !translate( sym, dir->lhs, bit_cnt, true ) ) return {};
					if ( !translate( sym, dir->rhs, bit_cnt, true ) ) return {};
					return dummy_expression;
				}

				// Handle casts as a redirect to resize.
				//
				if ( dir->op == math::operator_id::ucast ||
					 dir->op == math::operator_id::cast )
				{
					auto lhs = translate( sym, dir->lhs, bit_cnt, speculative_condition );
					if ( !lhs )	return {};
					auto rhs = translate( sym, dir->rhs, bit_cnt, speculative_condition );
					if ( !rhs ) return {};

					if ( auto sz = rhs->get<bitcnt_t>() )
					{
						( +lhs )->resize( sz.value(), dir->op == math::operator_id::cast );
						return lhs;
					}
					unreachable();
				}
				// If operation is binary:
				//
				else if ( dir->lhs )
				{
					auto lhs = translate( sym, dir->lhs, bit_cnt, speculative_condition );
					if ( !lhs )	return {};
					auto rhs = translate( sym, dir->rhs, bit_cnt, speculative_condition );
					if ( !rhs ) return {};
					return expression::make( lhs, dir->op, rhs );
				}
				// If operation is unary:
				//
				else
				{
					auto rhs = translate( sym, dir->rhs, bit_cnt, speculative_condition );
					if ( !rhs ) return {};
					return expression::make( dir->op, rhs );
				}
			}
			unreachable();
		}

		// If directive operator:
		//
		switch ( directive_op_desc{ dir->op }.value )
		{
			case directive_op_desc::simplify:
			{
				// If expression translates successfully: (Unset speculative_condition flag)
				//
				if ( auto e1 = translate( sym, dir->rhs, bit_cnt, false ) )
				{
					// Return only if it was successful.
					//
					if ( !e1->simplify_hint && simplify_expression( e1 ) )
						return e1;
				}
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_RED>( "Rejected, does not simplify.\n", dir->rhs->to_string() );
#endif
				break;
			}
			case directive_op_desc::try_simplify:
			{
				// Translate right hand side.
				//
				if ( auto e1 = translate( sym, dir->rhs, bit_cnt, speculative_condition ) )
				{
					// Simplify the expression if not dummy (generated by speculative checks).
					//
					if ( !speculative_condition )
						simplify_expression( e1 );
					return e1;
				}
				break;
			}
			case directive_op_desc::or_also:
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_BLU>( "Or directive hit %s.\n" );
				log<CON_BLU>( "Trying [%s]...\n", dir->lhs->to_string() );
#endif

				// Unpack first expression, if translated successfully, return it as is.
				//
				if ( auto e1 = translate( sym, dir->lhs, bit_cnt, speculative_condition ) )
					return e1;

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_BLU>( "Trying [%s]...\n", dir->rhs->to_string() );
#endif

				// Unpack second expression, if translated successfully, return it as is.
				//
				if ( auto e2 = translate( sym, dir->rhs, bit_cnt, speculative_condition ) )
					return e2;

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_RED>( "Both alternatives failed\n" );
#endif
				break;
			}
			case directive_op_desc::iff:
			{
				// Translate left hand side, if failed to do so or is not equal to [true], fail.
				//
				auto condition_status = translate( sym, dir->lhs, bit_cnt, false );
				if ( !condition_status || !( +condition_status )->simplify().get().value_or( false ) )
				{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					log<CON_RED>( "Rejected %s, condition (%s) not met.\n", dir->rhs->to_string(), dir->lhs->to_string() );
#endif
					return {};
				}

				// Continue the translation from the right hand side.
				//
				return translate( sym, dir->rhs, bit_cnt, speculative_condition );
			}
			case directive_op_desc::mask_unknown:
			{
				// Translate right hand side.
				//
				if ( auto exp = translate( sym, dir->rhs, bit_cnt, speculative_condition ) )
				{
					// Return the unknown mask.
					//
					return expression{ exp->unknown_mask(), exp->size() };
				}
				break;
			}
			case directive_op_desc::mask_one:
			{
				// Translate right hand side.
				//
				if ( auto exp = translate( sym, dir->rhs, bit_cnt, speculative_condition ) )
				{
					// Return the unknown mask.
					//
					return expression{ exp->known_one(), exp->size() };
				}
				break;
			}
			case directive_op_desc::mask_zero:
			{
				// Translate right hand side.
				//
				if ( auto exp = translate( sym, dir->rhs, bit_cnt, speculative_condition ) )
				{
					// Return the unknown mask.
					//
					return expression{ exp->known_zero(), exp->size() };
				}
				break;
			}
			case directive_op_desc::unreachable:
			{
				// Print an error.
				//
				error( "Directive-time assertation failure!\n" );
			}
			case directive_op_desc::warning:
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				// Print a warning.
				//
				log<CON_YLW>( "WARNING!\n" );
#endif

				// Continue the translation from the right hand side.
				//
				return translate( sym, dir->rhs, bit_cnt, speculative_condition );
			}
			default:
				unreachable();
		}

		// Failed translating the directive.
		//
		return {};
	}

	// Attempts to transform the expression in form A to form B as indicated by the directives, 
	// and returns the first instance that matches query.
	//
	expression::reference transform( const expression::reference& exp,
									 const instance::reference& from, const instance::reference& to,
									 const expression_filter_t& filter )
	{
		using namespace logger;

		// Match the expresison.
		//
		stack_vector<symbol_table_t> results;
		if ( !fast_match( &results, from, exp ) ) return {};

		// If a filter is provided:
		//
		if ( filter )
		{
			// For each possible match:
			//
			for ( auto& match : results )
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				// Log the translation.
				//
				log<CON_BLU>( "Translating [%s] => [%s]:\n", from->to_string(), to->to_string() );
				from->enum_variables( [ & ] ( const instance& ins )
				{
					log<CON_BLU>( "            %s: %s\n", ins.id, match.translate( ins )->to_string() );
				} );
#endif

				// If we could translate the directive:
				//
				if ( auto exp_new = translate( match, to, exp->size() ) )
				{
					// If it passes through the filter:
					//
					if ( filter( exp_new ) )
					{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
						// Log state and return the expression.
						//
						log<CON_GRN>( "Success.\n" );
#endif
						return exp_new;
					}

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					// Log state.
					//
					log<CON_RED>( "Rejected by filter (Complexity: %lf vs %lf).\n", exp_new->complexity, exp->complexity );
#endif
				}
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				// Otherwise, log state.
				//
				else
				{
					log<CON_RED>( "Rejected by directive.\n" );
				}
#endif
			}
		}
		else
		{
			// For each possible match:
			//
			for ( auto& match : results )
			{
				// Speculatively match, skip if fails.
				//
				if ( !translate( match, to, exp->size(), true ) )
				{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					// Log state.
					//
					log<CON_RED>( "Rejected by directive.\n" );
#endif
					continue;
				}

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				// Log the translation.
				//
				log<CON_BLU>( "Translating [%s] => [%s]:\n", from->to_string(), to->to_string() );
				from->enum_variables( [ & ] ( const instance& ins )
				{
					log<CON_BLU>( "            %s: %s\n", ins.id, match.translate( ins )->to_string() );
				} );
#endif

				// Translate the whole expression.
				//
				auto exp_new = translate( match, to, exp->size() );

				// Assert it was translated without failure since we speculatively 
				// checked the conditions.
				//
				fassert( exp_new );

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				// Log state and return the expression.
				//
				log<CON_GRN>( "Success.\n" );
#endif
				return exp_new;
			}
		}

		// Indicate failure with null reference.
		//
		return {};
	}
};