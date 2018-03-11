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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "move.h"
#include "types.h"
#include "transposition.h"

TransTable Table;

void initializeTranspositionTable(TransTable* table, uint64_t megabytes){
    
    // Minimum table size is 1MB. This maps to a key of size 15.
    // We start at 16, because the loop to adjust the memory
    // size to a power of two ends with a decrement of keySize
    uint64_t keySize = 16ull;
    
    // Every bucket must be 256 bits for the following scaling
    assert(sizeof(TransBucket) == 32);

    // Scale down the table to the closest power of 2, at or below megabytes
    for (;1ull << (keySize + 5) <= megabytes << 20 ; keySize++);
    keySize -= 1;
    
    // Setup Table's data members
    table->buckets      = calloc(1ull << keySize, sizeof(TransBucket));
    table->numBuckets   = 1ull << keySize;
    table->keySize      = keySize;
    table->generation   = 0u;
}

void destroyTranspositionTable(TransTable* table){
    free(table->buckets);
}

void updateTranspositionTable(TransTable* table){
    table->generation = (table->generation + 1) % 64;
}

void clearTranspositionTable(TransTable* table){
    
    unsigned int i; int j;
    TransEntry* entry;
    
    table->generation = 0u;
    
    for (i = 0u; i < table->numBuckets; i++){
        for (j = 0; j < BUCKET_SIZE; j++){
            entry = &(table->buckets[i].entries[j]);
            entry->value = 0;
            entry->depth = 0u;
            entry->info = 0u;
            entry->bestMove = 0u;
            entry->hash16 = 0u;
        }
    }
}

int estimateHashfull(TransTable* table){
    
    int i, used = 0;
    
    for (i = 0; i < 1250 && i < (int64_t)table->numBuckets; i++)
        used += (TransEntryType(table->buckets[i].entries[0]) != 0)
             +  (TransEntryType(table->buckets[i].entries[1]) != 0)
             +  (TransEntryType(table->buckets[i].entries[2]) != 0)
             +  (TransEntryType(table->buckets[i].entries[3]) != 0);
             
    return 1000 * used / (i * 4);
}

int getTranspositionEntry(TransTable* table, uint64_t hash, TransEntry* ttEntry){
    
    TransBucket* bucket = &(table->buckets[hash & (table->numBuckets - 1)]);
    int i; uint16_t hash16 = hash >> 48;
    
    #ifdef TEXEL
    return NULL;
    #endif
    
    // Search for a matching entry. Update the generation if found.
    for (i = 0; i < BUCKET_SIZE; i++){
        if (bucket->entries[i].hash16 == hash16){
            bucket->entries[i].info = (table->generation << 2) | TransEntryType(bucket->entries[i]);
            memcpy(ttEntry, &bucket->entries[i], sizeof(TransEntry));
            return 1;
        }
    }
    
    return 0;
}

void storeTranspositionEntry(TransTable* table, int depth, int type, int value, int bestMove, uint64_t hash){
    
    // Validate Parameters
    assert(depth < MAX_DEPTH && depth >= 0);
    assert(type == PVNODE || type == CUTNODE || type == ALLNODE);
    assert(value <= MATE && value >= -MATE);
    
    TransBucket* bucket = &(table->buckets[hash & (table->numBuckets - 1)]);
    TransEntry* entries = bucket->entries;
    TransEntry* replace = NULL;
    
    int i; uint16_t hash16 = hash >> 48;
    
    replace = &entries[0];
    
    for (i = 0; i < BUCKET_SIZE; i++){
        
        // Found an unused entry
        if (TransEntryType(entries[i]) == 0){
            replace = &(entries[i]);
            break;
        }
        
        // Found an entry with the same hash key
        if (entries[i].hash16 == hash16){
            replace = &(entries[i]);
            break;
        }
        
        if (   replace->depth   - (64 + table->generation - TransEntryAge(  *replace)) * 2
            >= entries[i].depth - (64 + table->generation - TransEntryAge(entries[i])) * 2)
            replace = &entries[i];
    }
    
    replace->value    = value;
    replace->depth    = depth;
    replace->info     = (table->generation << 2) | type;
    replace->bestMove = bestMove;
    replace->hash16   = hash16;
}

PawnKingEntry * getPawnKingEntry(PawnKingTable* pktable, uint64_t pkhash){
    PawnKingEntry* pkentry = &(pktable->entries[pkhash >> 48]);
    return pkentry->pkhash == pkhash ? pkentry : NULL;
}

void storePawnKingEntry(PawnKingTable* pktable, uint64_t pkhash, uint64_t passed, int mg, int eg){
    PawnKingEntry* pkentry = &(pktable->entries[pkhash >> 48]);
    pkentry->pkhash = pkhash;
    pkentry->passed = passed;
    pkentry->mg     = mg;
    pkentry->eg     = eg;
}
