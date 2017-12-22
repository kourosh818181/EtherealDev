/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>
  
  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <pthread.h>
#include <setjmp.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bitutils.h"
#include "bitboards.h"
#include "board.h"
#include "castle.h"
#include "evaluate.h"
#include "history.h"
#include "piece.h"
#include "psqt.h"
#include "search.h"
#include "thread.h"
#include "transposition.h"
#include "types.h"
#include "time.h"
#include "move.h"
#include "movegen.h"
#include "movepicker.h"
#include "uci.h"

extern TransTable Table;

uint16_t getBestMove(Thread* threads, Board* board, Limits* limits, double time, double inc, double mtg){
    
    int i, nthreads = threads[0].nthreads;
    
    double idealusage = 0, maxusage = 0, starttime = getRealTime();
    
    SearchInfo info; memset(&info, 0, sizeof(SearchInfo));
    
    pthread_t* pthreads = malloc(sizeof(pthread_t) * nthreads);
    
    // Ethereal is responsible for choosing how much time to spend searching
    if (limits->limitedBySelf){
        idealusage = mtg >= 0 ? 0.5 * (time / (mtg + 3)) : 0.5 * (time / 30);
        maxusage   = mtg >= 0 ? 2.4 * (time / (mtg + 1)) : inc + (time / 15);
        idealusage = MIN(idealusage, time - 20);
        maxusage   = MIN(maxusage,   time - 20);
    }
    
    // UCI command told us to look for exactly X seconds
    if (limits->limitedByTime){
        idealusage = limits->timeLimit;
        maxusage   = limits->timeLimit;
    }
    
    // Setup the thread pool for a new search with these parameters
    newSearchThreadPool(threads, board, limits, &info, starttime, &idealusage, maxusage);
    
    // Launch all of the threads
    for (i = 1; i < nthreads; i++)
        pthread_create(&pthreads[i], NULL, &iterativeDeepening, &threads[i]);
    
    // Wait for all threads to finish
    iterativeDeepening((void*) &threads[0]);
    for (i = 1; i < nthreads; i++)
        pthread_join(pthreads[i], NULL);
    
    // Cleanup pthreads
    free(pthreads);
    
    // Return highest depth best move
    return info.bestmoves[info.depth];
}

void* iterativeDeepening(void* vthread){
    
    Thread* const thread = (Thread*) vthread;
    
    SearchInfo* const info = thread->info;
    
    int i, count, value = 0, depth, abort, lastDepth;
    
    for (depth = 1; depth < MAX_DEPTH; depth++){
        
        // Determine if this thread should be running on at a higher depth
        
        pthread_mutex_lock(thread->lock);
        
        thread->depth = depth;
        
        for (count = 0, i = 0; i < thread->nthreads; i++)
            count += thread != &thread->threads[i] && thread->threads[i].depth >= depth;

        if (depth > 1 && thread->nthreads > 1 && count >= thread->nthreads / 2){
            thread->depth = depth + 1;
            pthread_mutex_unlock(thread->lock);
            continue;
        }

        pthread_mutex_unlock(thread->lock);
        
        abort = setjmp(thread->jbuffer);
        
        if (abort == ABORT_NONE){
            
            value = aspirationWindow(thread, depth);
            
            pthread_mutex_lock(thread->lock);
            
            // It is possible we finish the search but another thread has already
            // finished the same depth we just have. In this case, we do not want
            // to print the same depth twice, so we simply continue iterations
            
            if (thread->abort == ABORT_DEPTH){
                thread->abort = ABORT_NONE;
                pthread_mutex_unlock(thread->lock);
                continue;
            }
            
            else if (thread->abort == ABORT_ALL){
                pthread_mutex_unlock(thread->lock);
                return NULL;
            }
            
            // Dynamically decide how much time we should be using
            if (thread->limits->limitedBySelf){
                
                // Increase our time if the score suddently dropped by eight centipawns
                if (depth >= 4 && info->values[info->depth] > value + 8)
                    *thread->idealusage = MIN(thread->maxusage, *thread->idealusage * 1.10);
                
                // Increase our time if the pv has changed across the last two iterations
                if (depth >= 4 && info->bestmoves[info->depth] != thread->pv.line[0])
                    *thread->idealusage = MIN(thread->maxusage, *thread->idealusage * 1.35);
            }
            
            // Update the Search Info structure for the main thread
            lastDepth = info->depth;
            info->depth = depth;
            info->values[depth] = value;
            info->bestmoves[depth] = thread->pv.line[0];
            info->timeUsage[depth] = getRealTime() - thread->starttime - (depth > 1 ? info->timeUsage[lastDepth] : 0);
            
            // Send information about this search to the interface
            uciReport(thread->threads, thread->starttime, depth, value, &thread->pv);
            
            // Abort any threads still searching this depth, or lower
            for (i = 0; i < thread->nthreads; i++)
                if (   thread->depth >= thread->threads[i].depth
                    && thread != &thread->threads[i])
                    thread->threads[i].abort = ABORT_DEPTH;
            
            // Check for termination by any of the possible limits
            if (   (thread->limits->limitedByDepth && depth >= thread->limits->depthLimit)
                || (thread->limits->limitedByTime  && getRealTime() - thread->starttime > thread->limits->timeLimit)
                || (thread->limits->limitedBySelf  && getRealTime() - thread->starttime > thread->maxusage)
                || (thread->limits->limitedBySelf  && getRealTime() - thread->starttime > *thread->idealusage)){
                
                for (i = 0; i < thread->nthreads; i++)
                    thread->threads[i].abort = ABORT_ALL;
                
                pthread_mutex_unlock(thread->lock);
                
                break;
            }
            
            // Check to see if we expect to be able to complete the next depth
            if (thread->limits->limitedBySelf){
                double lastTime = info->timeUsage[depth];
                double timeFactor = MIN(2, info->timeUsage[depth] / info->timeUsage[lastDepth]);
                double estimatedUsage = lastTime * (timeFactor + .25);
                double estiamtedEndtime = getRealTime() + estimatedUsage - thread->starttime;
                
                if (estiamtedEndtime > thread->maxusage){
                    for (i = 0; i < thread->nthreads; i++)
                        thread->threads[i].abort = ABORT_ALL;
                    
                    pthread_mutex_unlock(thread->lock);
                    
                    break;
                }
            }
                
            
            pthread_mutex_unlock(thread->lock);
        }
        
        else if (abort == ABORT_DEPTH){
            pthread_mutex_lock(thread->lock);
            thread->abort = ABORT_NONE;
            memcpy(&thread->board, thread->initialboard, sizeof(Board));
            pthread_mutex_unlock(thread->lock);
        }
        
        else if (abort == ABORT_ALL){
            return NULL;
        }
    }
    
    return NULL;
}

int aspirationWindow(Thread* thread, int depth){
    
    int alpha, beta, value, margin;
    
    int* const values = thread->info->values;
    
    if (depth > 4 && abs(values[depth - 1]) < MATE / 2){
        
        margin =             1.6 * (abs(values[depth - 1] - values[depth - 2]));
        margin = MAX(margin, 2.0 * (abs(values[depth - 2] - values[depth - 3])));
        margin = MAX(margin, 0.8 * (abs(values[depth - 3] - values[depth - 4])));
        margin = MAX(margin, 1);
        
        for (; margin <= 640; margin *= 2){
            
            // Create the aspiration window
            thread->lower = alpha = values[depth - 1] - margin;
            thread->upper = beta  = values[depth - 1] + margin;
            
            // Perform the search on the modified window
            thread->value = value = search(thread, &thread->pv, alpha, beta, depth, 0);
            
            // Result was within our window
            if (value > alpha && value < beta)
                return value;
            
            // Result was a near mate score, force a full search
            if (abs(value) > MATE / 2)
                break;
        }
    }
    
    // Full window search ( near mate or when depth equals one )
    return search(thread, &thread->pv, -MATE, MATE, depth, 0);
}

int search(Thread* thread, PVariation* pv, int alpha, int beta, int depth, int height){
    
    const int PvNode   = (alpha != beta - 1);
    const int RootNode = (height == 0);
    
    Board* const board = &thread->board;
    
    int i, value, inCheck = 0, isQuiet, R, repetitions;
    int rAlpha, rBeta, ttValue, oldAlpha = alpha;
    int quiets = 0, played = 0, ttTactical = 0; 
    int best = -MATE, eval = -MATE, futilityMargin = -MATE;
    int hist = 0; // Fix bogus GCC warning
    
    uint16_t currentMove, quietsTried[MAX_MOVES];
    uint16_t ttMove = NONE_MOVE, bestMove = NONE_MOVE;
    
    Undo undo[1];
    EvalInfo ei;
    PVariation lpv;
    TransEntry ttEntry;
    MovePicker movePicker;
    
    lpv.length = 0;
    pv->length = 0;
    
    // Step 1A. Check to see if search time has expired
    if (   (thread->limits->limitedBySelf || thread->limits->limitedByTime)
        && (thread->nodes & 8191) == 8191
        &&  getRealTime() >= thread->starttime + thread->maxusage)
        longjmp(thread->jbuffer, ABORT_ALL);
        
    // Step 1B. Check to see if the master thread finished
    if (thread->abort) longjmp(thread->jbuffer, thread->abort);
    
    // Step 2. Distance Mate Pruning. Check to see if this line is so
    // good, or so bad, that being mated in the ply, or  mating in 
    // the next one, would still not create a more extreme line
    rAlpha = alpha > -MATE + height     ? alpha : -MATE + height;
    rBeta  =  beta <  MATE - height - 1 ?  beta :  MATE - height - 1;
    if (rAlpha >= rBeta) return rAlpha;
    
    // Step 3. Check for the Fifty Move Rule
    if (board->fiftyMoveRule > 100)
        return 0;
    
    // Step 4. Check for three fold repetition. If the repetition occurs since
    // the root move of this search, we will exit early as if it was a draw.
    // Otherwise, we will look for an actual three fold repetition draw.
    for (repetitions = 0, i = board->numMoves - 2; i >= 0; i -= 2){
        
        // We can't have repeated positions before the most recent
        // move which triggered a reset of the fifty move rule counter
        if (i < board->numMoves - board->fiftyMoveRule) break;
        
        if (board->history[i] == board->hash){
            
            // Repetition occured after the root
            if (i > board->numMoves - height)
                return 0;
            
            // An actual three fold repetition
            if (++repetitions == 2)
                return 0;
        }
    }
    
    // Step 5. Go into the Quiescence Search if we have reached
    // the search horizon and are not currently in check
    if (depth <= 0){
        inCheck = !isNotInCheck(board, board->turn);
        if (!inCheck) return qsearch(thread, pv, alpha, beta, height);
        
        // We do not cap reductions, so here we will make
        // sure that depth is within the acceptable bounds
        depth = 0; 
    }
    
    // If we did not exit already, we will call this a node
    thread->nodes += 1;
    
    // Step 6. Probe the Transposition Table for an entry
    if (getTranspositionEntry(&Table, board->hash, &ttEntry)){
        
        // Entry move may be good in this position. If it is tactical,
        // we may use it to increase reductions later on in LMR.
        ttMove = ttEntry.bestMove;
        ttTactical = moveIsTactical(board, ttMove);
        
        // Step 6A. Check to see if this entry allows us to exit this
        // node early. We choose not to do this in the PV line, not because
        // we can't, but because don't want truncated PV lines
        if (!PvNode && ttEntry.depth >= depth){

            rAlpha = alpha; rBeta = beta;
            ttValue = valueFromTT(ttEntry.value, height);
            
            switch (ttEntry.type){
                case  PVNODE: return ttValue;
                case CUTNODE: rAlpha = ttValue > alpha ? ttValue : alpha; break;
                case ALLNODE:  rBeta = ttValue <  beta ? ttValue :  beta; break;
            }
            
            // Entry allows early exit
            if (rAlpha >= rBeta) return ttValue;
        }
    }
    
    // Step 7. Determine check status, and calculate the futility margin.
    // We only need the futility margin if we are not in check, and we
    // are not looking at a PV Node, as those are not subject to futility.
    // Determine check status if not done already
    inCheck = inCheck || !isNotInCheck(board, board->turn);
    if (!PvNode){
        eval = evaluateBoard(board, &ei, &thread->ptable);
        futilityMargin = eval + depth * 0.95 * PieceValues[PAWN][EG];
    }
    
    // Step 8. Razoring. If a Quiescence Search for the current position
    // still falls way below alpha, we will assume that the score from
    // the Quiescence search was sufficient. For depth 1, we will just
    // return a Quiescence Search score because it is unlikely a quiet
    // move would close the massive gap between the evaluation and alpha
    if (   !PvNode
        && !inCheck
        &&  depth <= RazorDepth
        &&  eval + RazorMargins[depth] < alpha){
            
        if (depth <= 1)
            return qsearch(thread, pv, alpha, beta, height);
        
        rAlpha = alpha - RazorMargins[depth];
        value = qsearch(thread, pv, rAlpha, rAlpha + 1, height);
        if (value <= rAlpha) return value;
    }
    
    // Step 9. Beta Pruning / Reverse Futility Pruning / Static Null
    // Move Pruning. If the eval is few pawns above beta then exit early
    if (   !PvNode
        && !inCheck
        &&  depth <= BetaPruningDepth
        &&  hasNonPawnMaterial(board, board->turn)){
            
        value = eval - depth * 0.95 * PieceValues[PAWN][EG];
        
        if (value > beta)
            return value;
    }
    
    // Step 10. Null Move Pruning. If our position is so good that
    // giving our opponent back-to-back moves is still not enough
    // for them to gain control of the game, we can be somewhat safe
    // in saying that our position is too good to be true
    if (   !PvNode
        && !inCheck
        &&  depth >= NullMovePruningDepth
        &&  eval >= beta
        &&  hasNonPawnMaterial(board, board->turn)
        &&  board->history[board->numMoves-1] != NULL_MOVE){
            
        R = MIN(7, 4 + depth / 6 + (eval - beta + 200) / 400); 
            
        applyNullMove(board, undo);
        
        value = -search(thread, &lpv, -beta, -beta + 1, depth - R, height + 1);
        
        revertNullMove(board, undo);
        
        if (value >= beta){
            if (value >= MATE - MAX_HEIGHT)
                value = beta;
            return value;
        }
    }
    
    // Step 11. Internal Iterative Deepening. Searching PV nodes without
    // a known good move can be expensive, so a reduced search first
    if (    PvNode
        &&  ttMove == NONE_MOVE
        &&  depth >= InternalIterativeDeepeningDepth){
        
        // Search with a reduced depth
        value = search(thread, &lpv, alpha, beta, depth-2, height);
        
        // Probe for the newly found move, and update ttMove / ttTactical
        if (getTranspositionEntry(&Table, board->hash, &ttEntry)){
            ttMove = ttEntry.bestMove;
            ttTactical = moveIsTactical(board, ttMove);
        }
    }
    
    // Step 12. Check Extension
    depth += inCheck && !RootNode && (PvNode || depth <= 6);
    
    initializeMovePicker(&movePicker, thread, ttMove, height, 0);
    
    while ((currentMove = selectNextMove(&movePicker, board)) != NONE_MOVE){
        
        // If this move is quiet we will save it to a list of attemped
        // quiets, and we will need a history score for pruning decisions
        if ((isQuiet = !moveIsTactical(board, currentMove))){
            quietsTried[quiets++] = currentMove;
            hist = getHistoryScore(thread->history, currentMove, board->turn, 128);
        }
        
        // Step 13. Futility Pruning. If our score is far below alpha,
        // and we don't expect anything from this move, skip it.
        if (   !PvNode
            &&  isQuiet
            &&  played >= 1
            &&  futilityMargin <= alpha
            &&  depth <= FutilityPruningDepth)
            continue;
        
        // Apply and validate move before searching
        applyMove(board, currentMove, undo);
        if (!isNotInCheck(board, !board->turn)){
            revertMove(board, currentMove, undo);
            continue;
        }
        
        // Step 14. Late Move Pruning / Move Count Pruning. If we have
        // tried many quiets in this position already, and we don't expect
        // anything from this move, we can undo it and move on.
        if (   !PvNode
            &&  isQuiet
            &&  played >= 1
            &&  depth <= LateMovePruningDepth
            &&  quiets > LateMovePruningCounts[depth]
            &&  isNotInCheck(board, board->turn)){
        
            revertMove(board, currentMove, undo);
            continue;
        }
        
        // Update counter of moves actually played
        played += 1;
    
        // Step 15. Late Move Reductions. We will search some moves at a
        // lower depth. If they look poor at a lower depth, then we will
        // move on. If they look good, we will search with a full depth.
        if (    played >= 4
            &&  depth >= 3
            &&  isQuiet){
            
            R  = 2;
            R += (played - 4) / 8;
            R += (depth  - 4) / 6;
            R += 2 * !PvNode;
            R += ttTactical && bestMove == ttMove;
            R -= hist / 24;
            R  = MIN(depth - 1, MAX(R, 1));
        }
        
        else {
            R = 1;
        }
        
        // Search the move with a possibly reduced depth, on a full or null window
        value =  (played == 1 || !PvNode)
               ? -search(thread, &lpv, -beta, -alpha, depth-R, height+1)
               : -search(thread, &lpv, -alpha-1, -alpha, depth-R, height+1);
               
        // If the search beat alpha, we may need to research, in the event that
        // the previous search was not the full window, or was a reduced depth
        value =  (value > alpha && (R != 1 || (played != 1 && PvNode)))
               ? -search(thread, &lpv, -beta, -alpha, depth-1, height+1)
               :  value;
        
        // REVERT MOVE FROM BOARD
        revertMove(board, currentMove, undo);
        
        // Improved current value
        if (value > best){
            best = value;
            bestMove = currentMove;
            
            // IMPROVED CURRENT LOWER VALUE
            if (value > alpha){
                alpha = value;
                
                // Update the Principle Variation
                pv->length = 1 + lpv.length;
                pv->line[0] = currentMove;
                memcpy(pv->line + 1, lpv.line, sizeof(uint16_t) * lpv.length);
            }
        }
        
        // IMPROVED AND FAILED HIGH
        if (alpha >= beta){
            
            // Update killer moves
            if (isQuiet && thread->killers[height][0] != currentMove){
                thread->killers[height][1] = thread->killers[height][0];
                thread->killers[height][0] = currentMove;
            }
            
            break;
        }
    }
    
    if (played == 0) return inCheck ? -MATE + height : 0;
    
    else if (best >= beta && !moveIsTactical(board, bestMove)){
        updateHistory(thread->history, bestMove, board->turn, 1, depth*depth);
        for (i = 0; i < quiets - 1; i++)
            updateHistory(thread->history, quietsTried[i], board->turn, 0, depth*depth);
    }
    
    storeTranspositionEntry(&Table, depth, (best > oldAlpha && best < beta)
                            ? PVNODE : best >= beta ? CUTNODE : ALLNODE,
                            valueToTT(best, height), bestMove, board->hash);
    return best;
}

int qsearch(Thread* thread, PVariation* pv, int alpha, int beta, int height){
    
    Board* const board = &thread->board;
    
    int eval, value, best, maxValueGain;
    uint16_t currentMove;
    Undo undo[1];
    MovePicker movePicker;
    EvalInfo ei;
    
    PVariation lpv;
    lpv.length = 0;
    pv->length = 0;
    
    // Step 1A. Check to see if search time has expired
    if (   (thread->limits->limitedBySelf || thread->limits->limitedByTime)
        && (thread->nodes & 8191) == 8191
        &&  getRealTime() >= thread->starttime + thread->maxusage)
        longjmp(thread->jbuffer, ABORT_ALL);
        
    // Step 1B. Check to see if the master thread finished
    if (thread->abort) longjmp(thread->jbuffer, thread->abort);
    
    // Call this a node
    thread->nodes += 1;
    
    // Max height reached, stop here
    if (height >= MAX_HEIGHT)
        return evaluateBoard(board, &ei, &thread->ptable);
    
    // Get a standing eval of the current board
    best = value = eval = evaluateBoard(board, &ei, &thread->ptable);
    
    // Update lower bound
    if (value > alpha) alpha = value;
    
    // QSearch can be terminated
    if (alpha >= beta) return value;
    
    // Take a guess at the best case gain for a non promotion capture
    if (board->colours[!board->turn] & board->pieces[QUEEN])
        maxValueGain = PieceValues[QUEEN ][EG] + 55;
    else if (board->colours[!board->turn] & board->pieces[ROOK])
        maxValueGain = PieceValues[ROOK  ][EG] + 35;
    else
        maxValueGain = PieceValues[BISHOP][EG] + 15;
    
    // Delta pruning when no promotions and not extreme late game
    if (     value + maxValueGain < alpha
        && !(board->colours[WHITE] & board->pieces[PAWN] & RANK_7)
        && !(board->colours[BLACK] & board->pieces[PAWN] & RANK_2))
        return value;
    
    
    initializeMovePicker(&movePicker, thread, NONE_MOVE, height, 1);
    
    while ((currentMove = selectNextMove(&movePicker, board)) != NONE_MOVE){
        
        // Take a guess at the best case value of this current move
        value = eval + 55 + PieceValues[PieceType(board->squares[MoveTo(currentMove)])][EG];
        if (MoveType(currentMove) == PROMOTION_MOVE){
            value += PieceValues[1 + (MovePromoType(currentMove) >> 14)][EG];
            value -= PieceValues[PAWN][EG];
        }
        
        // If the best case is not good enough, continue
        if (value < alpha)
            continue;
        
        // Prune this capture if it is capturing a weaker piece which is protected,
        // so long as we do not have any additional support for the attacker. If
        // the capture is also a promotion we will not perform any pruning here
        if (     MoveType(currentMove) != PROMOTION_MOVE
            &&  (ei.attacked[!board->turn]   & (1ull << MoveTo(currentMove)))
            && !(ei.attackedBy2[board->turn] & (1ull << MoveTo(currentMove)))
            &&  PieceValues[PieceType(board->squares[MoveTo  (currentMove)])][MG]
             <  PieceValues[PieceType(board->squares[MoveFrom(currentMove)])][MG])
            continue;
        
        // Apply and validate move before searching
        applyMove(board, currentMove, undo);
        if (!isNotInCheck(board, !board->turn)){
            revertMove(board, currentMove, undo);
            continue;
        }
        
        // Search next depth
        value = -qsearch(thread, &lpv, -beta, -alpha, height+1);
        
        // Revert move from board
        revertMove(board, currentMove, undo);
        
        // Improved current value
        if (value > best){
            best = value;
            
            // Improved current lower bound
            if (value > alpha){
                alpha = value;
                
                // Update the Principle Variation
                pv->length = 1 + lpv.length;
                pv->line[0] = currentMove;
                memcpy(pv->line + 1, lpv.line, sizeof(uint16_t) * lpv.length);
            }
        }
        
        // Search has failed high
        if (alpha >= beta)
            return best;
    }
    
    return best;
}

int moveIsTactical(Board* board, uint16_t move){
    return board->squares[MoveTo(move)] != EMPTY
        || MoveType(move) == PROMOTION_MOVE
        || MoveType(move) == ENPASS_MOVE;
}

int hasNonPawnMaterial(Board* board, int turn){
    uint64_t friendly = board->colours[turn];
    uint64_t kings = board->pieces[KING];
    uint64_t pawns = board->pieces[PAWN];
    return (friendly & (kings | pawns)) != friendly;
}

int valueFromTT(int value, int height){
    return value >=  MATE - MAX_HEIGHT ? value - height
         : value <= -MATE + MAX_HEIGHT ? value + height
         : value;
}

int valueToTT(int value, int height){
    return value >=  MATE - MAX_HEIGHT ? value + height
         : value <= -MATE + MAX_HEIGHT ? value - height
         : value;
}
