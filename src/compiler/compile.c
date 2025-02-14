
/* +--------------------------------------------------------------------------+
** |                          _   _       _                                   |
** |                         | \ | |     (_)                                  |
** |                         |  \| | ___  _  __ _                             |
** |                         | . ` |/ _ \| |/ _` |                            |
** |                         | |\  | (_) | | (_| |                            |
** |                         |_| \_|\___/| |\__,_|                            |
** |                                    _/ |                                  |
** |                                   |__/                                   |
** +--------------------------------------------------------------------------+
** | Copyright (c) 2022 Francesco Cozzuto <francesco.cozzuto@gmail.com>       |
** +--------------------------------------------------------------------------+
** | This file is part of The Noja Interpreter.                               |
** |                                                                          |
** | The Noja Interpreter is free software: you can redistribute it and/or    |
** | modify it under the terms of the GNU General Public License as published |
** | by the Free Software Foundation, either version 3 of the License, or (at |
** | your option) any later version.                                          |
** |                                                                          |
** | The Noja Interpreter is distributed in the hope that it will be useful,  |
** | but WITHOUT ANY WARRANTY; without even the implied warranty of           |
** | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General |
** | Public License for more details.                                         |
** |                                                                          |
** | You should have received a copy of the GNU General Public License along  |
** | with The Noja Interpreter. If not, see <http://www.gnu.org/licenses/>.   |
** +--------------------------------------------------------------------------+ 
** |                         WHAT IS THIS FILE?                               |
** |                                                                          |
** | This file implements the routines that transform the AST into a list of  |
** | bytecodes. The functionalities of this file are exposed through the      |
** | `compile` function, that takes as input an `AST` and outputs an          |
** | `Executable`.                                                            |
** |                                                                          |
** | The function that does the heavy lifting is `emit_instr_for_node` which  |
** | walks the tree and writes instructions to the `ExeBuilder`.              |
** |                                                                          |
** | Some semantic errors are catched at this phase, in which case, they are  |
** | reported by filling out the `error` structure and aborting. It's also    |
** | possible that the compilation fails bacause of internal errors (which    |
** | usually means "out of memory").                                          |
** +--------------------------------------------------------------------------+
*/

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include "../utils/defs.h"
#include "compile.h"
#include "ASTi.h"

static _Bool emit_instr_for_node(ExeBuilder *exeb, Node *node, Promise *break_dest, Error *error);

static Opcode exprkind_to_opcode(ExprKind kind)
{
	switch(kind)
	{
		case EXPR_NOT: return OPCODE_NOT;
		case EXPR_POS: return OPCODE_POS;
		case EXPR_NEG: return OPCODE_NEG;
		case EXPR_ADD: return OPCODE_ADD;
		case EXPR_SUB: return OPCODE_SUB;
		case EXPR_MUL: return OPCODE_MUL;
		case EXPR_DIV: return OPCODE_DIV;
		case EXPR_EQL: return OPCODE_EQL;
		case EXPR_NQL: return OPCODE_NQL;
		case EXPR_LSS: return OPCODE_LSS;
		case EXPR_LEQ: return OPCODE_LEQ;
		case EXPR_GRT: return OPCODE_GRT;
		case EXPR_GEQ: return OPCODE_GEQ;
		case EXPR_AND: return OPCODE_AND;
		case EXPR_OR:  return OPCODE_OR;
		default:
		UNREACHABLE;
		break;
	}
}

static _Bool emit_instr_for_funccall(ExeBuilder *exeb, CallExprNode *expr, Promise *break_dest, int returns, Error *error)
{
	Node *arg = expr->argv;
    
	while(arg)
	{
		if(!emit_instr_for_node(exeb, arg, break_dest, error))
			return 0;

		arg = arg->next;
	}

	if(!emit_instr_for_node(exeb, expr->func, break_dest, error))
		return 0;

	Operand ops[2];
	ops[0] = (Operand) { .type = OPTP_INT, .as_int = expr->argc };
	ops[1] = (Operand) { .type = OPTP_INT, .as_int = returns };
	return ExeBuilder_Append(exeb, error, OPCODE_CALL, ops, 2, expr->base.base.offset, expr->base.base.length);
}

static _Bool flatten_tuple_tree(ExprNode *root, ExprNode **tuple, int max, int *count, Error *error)
{
	if(root->kind == EXPR_PAIR)
		return flatten_tuple_tree((ExprNode*) ((OperExprNode*) root)->head, tuple, max, count, error) && 
				flatten_tuple_tree((ExprNode*) ((OperExprNode*) root)->head->next, tuple, max, count, error);

	if(max == *count)
	{
		Error_Report(error, 0, "Static buffer is too small");
		return 0;
	}

	tuple[(*count)++] = root;
	return 1;
}

static _Bool emit_instr_for_node(ExeBuilder *exeb, Node *node, Promise *break_dest, Error *error)
{
	assert(node != NULL);

	switch(node->kind)
	{
		case NODE_EXPR:
		{
			ExprNode *expr = (ExprNode*) node;
			switch(expr->kind)
			{
				case EXPR_PAIR:
				Error_Report(error, 0, "Tuple outside of assignment or return statement");
				return 0;

				case EXPR_NOT:
				case EXPR_POS:
				case EXPR_NEG:
				case EXPR_ADD:
				case EXPR_SUB:
				case EXPR_MUL:
				case EXPR_DIV:
				case EXPR_EQL:
				case EXPR_NQL:
				case EXPR_LSS:
				case EXPR_LEQ:
				case EXPR_GRT:
				case EXPR_GEQ:
				case EXPR_AND:
				case EXPR_OR:
				{
					OperExprNode *oper = (OperExprNode*) expr;

					for(Node *operand = oper->head; operand; operand = operand->next)
						if(!emit_instr_for_node(exeb, operand, break_dest, error))
							return 0;

					if(!ExeBuilder_Append(exeb, error,
						exprkind_to_opcode(expr->kind), 
						NULL, 0, node->offset, node->length))
						return 0;
					return 1;
				}

				case EXPR_ASS:
				{
					OperExprNode *oper = (OperExprNode*) expr;

					Node *lop, *rop;
					lop = oper->head;
					rop = lop->next;

					ExprNode *tuple[32];
					int count = 0;

					if(!flatten_tuple_tree((ExprNode*) lop, tuple, sizeof(tuple)/sizeof(tuple[0]), &count, error))
						return 0;

					assert(count > 0);

					if(count == 1) /* No tuple. */
					{
						if(!emit_instr_for_node(exeb, rop, break_dest, error))
							return 0;
					}
					else
					{
						if(((ExprNode*) rop)->kind == EXPR_CALL)
						{
							if(!emit_instr_for_funccall(exeb, (CallExprNode*) rop, break_dest, count, error))
								return 0;
						}
						else
						{
							Error_Report(error, 0, "Assigning to %d variables only 1 value", count);
							return 0;
						}
					}
					
					for(int i = 0; i < count; i += 1)
					{
						ExprNode *tuple_item = tuple[count-i-1];
						switch(tuple_item->kind)
						{
							case EXPR_IDENT:
							{
								const char *name = ((IdentExprNode*) tuple_item)->val;

								Operand op = { .type = OPTP_STRING, .as_string = name };
								if(!ExeBuilder_Append(exeb, error, OPCODE_ASS, &op, 1, tuple_item->base.offset, tuple_item->base.length))
									return 0;
								break;
							}

							case EXPR_SELECT:
							{
								Node *idx = ((IndexSelectionExprNode*) tuple_item)->idx;
								Node *set = ((IndexSelectionExprNode*) tuple_item)->set;

								if(!emit_instr_for_node(exeb, set, break_dest, error))
									return 0;

								if(!emit_instr_for_node(exeb, idx, break_dest, error))
									return 0;

								if(!ExeBuilder_Append(exeb, error, OPCODE_INSERT2, NULL, 0, tuple_item->base.offset, tuple_item->base.length))
									return 0;
								break;
							}

							default:
							Error_Report(error, 0, "Assigning to something that it can't be assigned to");
							return 0;
						}

						if(i+1 < count)
						{
							Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
							if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, node->offset, 0))
								return 0;
						}
					}

					return 1;
				}

				case EXPR_INT:
				{
					IntExprNode *p = (IntExprNode*) expr;
					Operand op = { .type = OPTP_INT, .as_int = p->val };
					return ExeBuilder_Append(exeb, error, OPCODE_PUSHINT, &op, 1, node->offset, node->length);
				}

				case EXPR_FLOAT:
				{
					FloatExprNode *p = (FloatExprNode*) expr;
					Operand op = { .type = OPTP_FLOAT, .as_float = p->val };
					return ExeBuilder_Append(exeb, error, OPCODE_PUSHFLT, &op, 1, node->offset, node->length);
				}

				case EXPR_STRING:
				{
					StringExprNode *p = (StringExprNode*) expr;
					Operand op = { .type = OPTP_STRING, .as_string = p->val };
					return ExeBuilder_Append(exeb, error, OPCODE_PUSHSTR, &op, 1, node->offset, node->length);
				}

				case EXPR_IDENT:
				{
					IdentExprNode *p = (IdentExprNode*) expr;
					Operand op = { .type = OPTP_STRING, .as_string = p->val };
					return ExeBuilder_Append(exeb, error, OPCODE_PUSHVAR, &op, 1, node->offset, node->length);
				}

				case EXPR_LIST:
				{
					// PUSHLST
					// PUSHINT
					// <expr>
					// INSERT

					ListExprNode *l = (ListExprNode*) node;

					Operand op;

					op = (Operand) { .type = OPTP_INT, .as_int = l->itemc };
					if(!ExeBuilder_Append(exeb, error, OPCODE_PUSHLST, &op, 1, node->offset, node->length))
						return 0;

					Node *item = l->items;
					int i = 0;

					while(item)
					{
						op = (Operand) { .type = OPTP_INT, .as_int = i };
						if(!ExeBuilder_Append(exeb, error, OPCODE_PUSHINT, &op, 1, item->offset, item->length))
							return 0;

						if(!emit_instr_for_node(exeb, item, break_dest, error))
							return 0;

						if(!ExeBuilder_Append(exeb, error, OPCODE_INSERT, NULL, 0, item->offset, item->length))
							return 0;
										
						i += 1;
						item = item->next;
					}
					return 1;
				}

				case EXPR_MAP:
				{
					MapExprNode *m = (MapExprNode*) node;

					Operand op;

					op = (Operand) { .type = OPTP_INT, .as_int = m->itemc };
					if(!ExeBuilder_Append(exeb, error, OPCODE_PUSHMAP, &op, 1, node->offset, node->length))
						return 0;

					Node *key  = m->keys;
					Node *item = m->items;
								
					while(item)
					{
						if(!emit_instr_for_node(exeb, key, break_dest, error))
							return 0;

						if(!emit_instr_for_node(exeb, item, break_dest, error))
							return 0;

						if(!ExeBuilder_Append(exeb, error, OPCODE_INSERT, NULL, 0, item->offset, item->length))
							return 0;
									
						key  =  key->next;
						item = item->next;
					}

					return 1;
				}

				case EXPR_CALL:
				return emit_instr_for_funccall(exeb, (CallExprNode*) expr, break_dest, 1, error);

				case EXPR_SELECT:
				{
					IndexSelectionExprNode *sel = (IndexSelectionExprNode*) expr;
					
					if(!emit_instr_for_node(exeb, sel->set, break_dest, error))
						return 0;

					if(!emit_instr_for_node(exeb, sel->idx, break_dest, error))
						return 0;

					return ExeBuilder_Append(exeb, error, OPCODE_SELECT, NULL, 0, node->offset, node->length);
				}

				case EXPR_NONE:
				return ExeBuilder_Append(exeb, error, OPCODE_PUSHNNE, NULL, 0, node->offset, node->length);

				case EXPR_TRUE:
				return ExeBuilder_Append(exeb, error, OPCODE_PUSHTRU, NULL, 0, node->offset, node->length);

				case EXPR_FALSE:
				return ExeBuilder_Append(exeb, error, OPCODE_PUSHFLS, NULL, 0, node->offset, node->length);

				default:
				UNREACHABLE;
				break;
			}
			break;
		}

		case NODE_BREAK:
		{
			if(break_dest == NULL)
			{
				Error_Report(error, 0, "Break not inside a loop");
				return 0;
			}
			Operand op = (Operand) { .type = OPTP_PROMISE, .as_promise = break_dest };
			if(!ExeBuilder_Append(exeb, error, OPCODE_JUMP, &op, 1, node->offset, node->length))
				return 0;
			return 1;
		}

		case NODE_IFELSE:
		{
			IfElseNode *ifelse = (IfElseNode*) node;

			if(!emit_instr_for_node(exeb, ifelse->condition, break_dest, error))
				return 0;

			if(ifelse->false_branch)
			{
				Promise *else_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));
				Promise *done_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));

				if(else_offset == NULL || done_offset == NULL)
				{
					Error_Report(error, 1, "No memory");
					return 0;
				}

				Operand op = { .type = OPTP_PROMISE, .as_promise = else_offset };
				if(!ExeBuilder_Append(exeb, error, OPCODE_JUMPIFNOTANDPOP, &op, 1, node->offset, node->length))
					return 0;

				if(!emit_instr_for_node(exeb, ifelse->true_branch, break_dest, error))
					return 0;

				if(ifelse->true_branch->kind == NODE_EXPR)
				{
					Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, ifelse->true_branch->offset, 0))
						return 0;
				}
						
				op = (Operand) { .type = OPTP_PROMISE, .as_promise = done_offset };
				if(!ExeBuilder_Append(exeb, error, OPCODE_JUMP, &op, 1, node->offset, node->length))
					return 0;

				long long int temp = ExeBuilder_InstrCount(exeb);
				Promise_Resolve(else_offset, &temp, sizeof(temp));

				if(!emit_instr_for_node(exeb, ifelse->false_branch, break_dest, error))
					return 0;

				if(ifelse->false_branch->kind == NODE_EXPR)
				{
					Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, ifelse->false_branch->offset, 0))
						return 0;
				}

				temp = ExeBuilder_InstrCount(exeb);
				Promise_Resolve(done_offset, &temp, sizeof(temp));

				Promise_Free(else_offset);
				Promise_Free(done_offset);
			}
			else
			{
				Promise *done_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));

				if(done_offset == NULL)
				{
					Error_Report(error, 1, "No memory");
					return 0;
				}

				if(!ExeBuilder_Append(exeb, error, OPCODE_JUMPIFNOTANDPOP, &(Operand) { .type = OPTP_PROMISE, .as_promise = done_offset }, 1, node->offset, node->length))
					return 0;

				if(!emit_instr_for_node(exeb, ifelse->true_branch, break_dest, error))
					return 0;

				if(ifelse->true_branch->kind == NODE_EXPR)
				{
					Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, ifelse->true_branch->offset, 0))
						return 0;
				}

				long long int temp = ExeBuilder_InstrCount(exeb);
				Promise_Resolve(done_offset, &temp, sizeof(temp));

				Promise_Free(done_offset);
			}

			return 1;
		}

		case NODE_WHILE:
		{
			WhileNode *whl = (WhileNode*) node;

			/* 
			 * start:
			 *   <condition>
			 * 	 JUMPIFNOTANDPOP end
			 *   <body>
			 *   JUMP start
			 * end:
			 */

			Promise *start_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));
			Promise   *end_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));

			if(start_offset == NULL || end_offset == NULL)
			{
				Error_Report(error, 1, "No memory");
				return 0;
			}

			long long int temp = ExeBuilder_InstrCount(exeb);
			Promise_Resolve(start_offset, &temp, sizeof(temp));

			if(!emit_instr_for_node(exeb, whl->condition, break_dest, error))
				return 0;

			Operand op = { .type = OPTP_PROMISE, .as_promise = end_offset };
			if(!ExeBuilder_Append(exeb, error, OPCODE_JUMPIFNOTANDPOP, &op, 1, whl->condition->offset, whl->condition->length))
				return 0;

			if(!emit_instr_for_node(exeb, whl->body, end_offset, error))
				return 0;

			if(whl->body->kind == NODE_EXPR)
			{
				Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
				if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, whl->body->offset, 0))
					return 0;
			}
				
			op = (Operand) { .type = OPTP_PROMISE, .as_promise = start_offset };
			if(!ExeBuilder_Append(exeb, error, OPCODE_JUMP, &op, 1, node->offset, node->length))
				return 0;

			temp = ExeBuilder_InstrCount(exeb);
			Promise_Resolve(end_offset, &temp, sizeof(temp));

			Promise_Free(start_offset);
			Promise_Free(  end_offset);
			return 1;
		}

		case NODE_DOWHILE:
		{
			DoWhileNode *dowhl = (DoWhileNode*) node;

			/*
			 * start:
			 *   <body>
			 *   <condition>
			 *   JUMPIFANDPOP start
			 */

			Promise *end_offset = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));
			if(end_offset == NULL)
			{
				Error_Report(error, 1, "No memory");
				return 0;
			}

			long long int start = ExeBuilder_InstrCount(exeb);

			if(!emit_instr_for_node(exeb, dowhl->body, end_offset, error))
				return 0;

			if(dowhl->body->kind == NODE_EXPR)
			{
				Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
				if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, dowhl->body->offset, 0))
					return 0;
			}

			if(!emit_instr_for_node(exeb, dowhl->condition, break_dest, error))
				return 0;

			Operand op = { .type = OPTP_INT, .as_int = start };
			if(!ExeBuilder_Append(exeb, error, OPCODE_JUMPIFANDPOP, &op, 1, dowhl->condition->offset, dowhl->condition->length))
				return 0;

			long long int temp = ExeBuilder_InstrCount(exeb);
			Promise_Resolve(end_offset, &temp, sizeof(temp));
			Promise_Free(end_offset);
			return 1;
		}

		case NODE_COMP:
		{
			CompoundNode *comp = (CompoundNode*) node;

			Node *stmt = comp->head;

			while(stmt)
			{
				if(!emit_instr_for_node(exeb, stmt, break_dest, error))
					return 0;

				if(stmt->kind == NODE_EXPR)
				{
					Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, stmt->offset, 0))
						return 0;
				}
				stmt = stmt->next;
			}

			return 1;
		}

		case NODE_RETURN:
		{
			ReturnNode *ret = (ReturnNode*) node;

			ExprNode *tuple[32];
			int count = 0;

			if(!flatten_tuple_tree((ExprNode*) ret->val, tuple, sizeof(tuple)/sizeof(tuple[0]), &count, error))
				return 0;

			for(int i = 0; i < count; i += 1)
				if(!emit_instr_for_node(exeb, (Node*) tuple[i], break_dest, error))
					return 0;

			Operand op = (Operand) { .type = OPTP_INT, .as_int = count };
			if(!ExeBuilder_Append(exeb, error, OPCODE_RETURN, &op, 1, ret->base.offset, ret->base.length))
				return 0;

			return 1;
		}

		case NODE_FUNC:
		{
			FunctionNode *func = (FunctionNode*) node;

			Promise *func_index = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));
			Promise *jump_index = Promise_New(ExeBuilder_GetAlloc(exeb), sizeof(long long int));

			if(func_index == NULL || jump_index == NULL)
			{
				Error_Report(error, 1, "No memory");
				return 0;
			}

			// Push function.
			{
				Operand ops[2] = {
					{ .type = OPTP_PROMISE, .as_promise = func_index },
					{ .type = OPTP_INT,     .as_int     = func->argc },
				};

				if(!ExeBuilder_Append(exeb, error, OPCODE_PUSHFUN, ops, 2, func->base.offset, func->base.length))
					return 0;
			}
				
			// Assign variable.
			Operand op = (Operand) { .type = OPTP_STRING, .as_string = func->name };
			if(!ExeBuilder_Append(exeb, error, OPCODE_ASS, &op, 1,  func->base.offset, func->base.length))
				return 0;

			// Pop function object.
			op = (Operand) { .type = OPTP_INT, .as_int = 1 };
			if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1,  func->base.offset, func->base.length))
				return 0;

			// Jump after the function code.
			op = (Operand) { .type = OPTP_PROMISE, .as_promise = jump_index };
			if(!ExeBuilder_Append(exeb, error, OPCODE_JUMP, &op, 1,  func->base.offset, func->base.length))
				return 0;

			// This is the function code index.
			long long int temp = ExeBuilder_InstrCount(exeb);
			Promise_Resolve(func_index, &temp, sizeof(temp));

			// Compile the function body.
			{
				// Assign the arguments.

				if(func->argv)
					{ assert(func->argv->kind == NODE_ARG); }

				ArgumentNode *arg = (ArgumentNode*) func->argv;

				while(arg)
				{
					op = (Operand) { .type = OPTP_STRING, .as_string = arg->name };
					if(!ExeBuilder_Append(exeb, error, OPCODE_ASS, &op, 1,  arg->base.offset, arg->base.length))
						return 0;

					op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1,  arg->base.offset, arg->base.length))
						return 0;

					if(arg->base.next)
						{ assert(arg->base.next->kind == NODE_ARG); }

					arg = (ArgumentNode*) arg->base.next;
				}

				if(!emit_instr_for_node(exeb, func->body, NULL, error))
					return 0;

				if(func->body->kind == NODE_EXPR)
				{
					Operand op = (Operand) { .type = OPTP_INT, .as_int = 1 };
					if(!ExeBuilder_Append(exeb, error, OPCODE_POP, &op, 1, func->body->offset + func->body->length, 0))
						return 0;
				}

				// Write a return instruction, just 
				// in case it didn't already return.
				Operand op = (Operand) { .type = OPTP_INT, .as_int = 0 };
				if(!ExeBuilder_Append(exeb, error, OPCODE_RETURN, &op, 1, func->body->offset, 0))
					return 0;
			}

			// This is the first index after the function code.
			temp = ExeBuilder_InstrCount(exeb);
			Promise_Resolve(jump_index, &temp, sizeof(temp));

			Promise_Free(func_index);
			Promise_Free(jump_index);
			return 1;
		}

		default:
		UNREACHABLE;
		return 0;
	}
	UNREACHABLE;
	return 0;
}

/* Symbol: compile
 * 
 *   Serializes an AST into bytecode format.
 *
 *
 * Arguments:
 *
 *   ast: The AST to be serialized.
 *   alloc: The allocator that will be used to get new
 *			memory. (optional)
 *   error: Error information structure that is filled out if
 *          an error occurres.
 *
 *
 * Returns:
 *   A pointer to an `Executable` that is the object that
 *	 contains the bytecode. If an error occurres, NULL is 
 *   returned and the `error` structure is filled out.
 *
 */
Executable *compile(AST *ast, BPAlloc *alloc, Error *error)
{
	assert(ast != NULL);
	assert(error != NULL);

	BPAlloc *alloc2 = alloc;

	if(alloc2 == NULL)
	{
		alloc2 = BPAlloc_Init(-1);

		if(alloc2 == NULL)
			return NULL;
	}

	Executable *exe = NULL;
	ExeBuilder *exeb = ExeBuilder_New(alloc2);

	if(exeb != NULL)
	{
		if(!emit_instr_for_node(exeb, ast->root, NULL, error))
			return 0;

		Operand op = (Operand) { .type = OPTP_INT, .as_int = 0 };
		if(ExeBuilder_Append(exeb, error, OPCODE_RETURN, &op, 1, Source_GetSize(ast->src), 0))
		{
			exe = ExeBuilder_Finalize(exeb, error);
					
			if(exe != NULL)
				Executable_SetSource(exe, ast->src);
		}
	}

	if(alloc == NULL)
		BPAlloc_Free(alloc2);
	return exe;
}