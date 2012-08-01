/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2012 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."
/* We are going to test whether create and close properly check their input. */

#include "test.h"

int r;
toku_lock_tree* lt  = NULL;
toku_ltm*       ltm = NULL;
DB*             db  = (DB*)1;
enum { MAX_LT_LOCKS = 10 };
uint32_t max_locks = MAX_LT_LOCKS;
uint64_t max_lock_memory = MAX_LT_LOCKS*256;
bool duplicates = false;
int  nums[10000];

DBT _keys_left[2];
DBT _keys_right[2];
DBT* keys_left[2]   ;
DBT* keys_right[2]  ;

toku_point qleft, qright;
toku_interval query;
toku_range* buf;
unsigned buflen;
unsigned numfound;

static void init_query(void) {  
    init_point(&qleft,  lt);
    init_point(&qright, lt);
    
    qleft.key_payload  = (void *) toku_lt_neg_infinity;
    qright.key_payload = (void *) toku_lt_infinity;

    memset(&query,0,sizeof(query));
    query.left  = &qleft;
    query.right = &qright;
}

static void setup_tree(void) {
    assert(!lt && !ltm);
    r = toku_ltm_create(&ltm, max_locks, max_lock_memory, dbpanic);
    CKERR(r);
    assert(ltm);
    //ask ltm for lock tree
    DICTIONARY_ID dict_id = {0x1234};
    r = toku_ltm_get_lt(ltm, &lt, dict_id, NULL, intcmp, NULL, NULL, NULL);

    CKERR(r);
    assert(lt);
    init_query();
}

static void close_tree(void) {
    assert(lt && ltm);

    toku_lt_remove_db_ref(lt);
    r = toku_ltm_close(ltm);
        CKERR(r);
    lt = NULL;
    ltm = NULL;
}

typedef enum { null = -1, infinite = -2, neg_infinite = -3 } lt_infty;

static DBT* set_to_infty(DBT *dbt, int value) {
    if (value == infinite) return (DBT*)toku_lt_infinity;
    if (value == neg_infinite) return (DBT*)toku_lt_neg_infinity;
    if (value == null) return dbt_init(dbt, NULL, 0);
    assert(0 <= value && (unsigned) value < sizeof nums / sizeof nums[0]);
    return                    dbt_init(dbt, &nums[value], sizeof(nums[0]));
}


static void lt_insert(int r_expect, char txn, int key_l, int key_r, bool read_flag) {
    DBT _key_left;
    DBT _key_right;
    DBT* key_left   = &_key_left;
    DBT* key_right  = &_key_right;

    key_left  = set_to_infty(key_left,  key_l);
    key_right = set_to_infty(key_right, key_r);
    {
        assert(key_left);
        assert(!read_flag || key_right);
    }

    TXNID local_txn = (TXNID) (size_t) txn;

    if (read_flag)
        r = toku_lt_acquire_range_read_lock(lt, local_txn, key_left, key_right);
    else
        r = toku_lt_acquire_write_lock(lt, local_txn, key_left);
    CKERR2(r, r_expect);
}

static int lt_insert_write_no_check(char txn, int key_p) {
    DBT key;
    TXNID local_txn = (TXNID) (size_t) txn;
    r = toku_lt_acquire_write_lock(lt, local_txn, dbt_init(&key, &nums[key_p], sizeof(nums[0])));
    return r;
}

static void lt_insert_read(int r_expect, char txn, int key_l, int key_r) {
    lt_insert(r_expect, txn, key_l, key_r, true);
}

static void lt_insert_write(int r_expect, char txn, int key_l) {
    lt_insert(r_expect, txn, key_l, 0, false);
}

static void lt_unlock(char ctxn) {
    int retval = toku_lt_unlock_txn(lt, (TXNID) (size_t) ctxn); CKERR(retval);
}
              
static void run_escalation_test(void) {
    int i = 0;
/* ******************** */
/* 1 transaction request 1000 write locks, make sure it succeeds*/
    setup_tree();
    assert(lt->lock_escalation_allowed);
    for (i = 0; i < 1000; i++) {
        lt_insert_write(0, 'a', i);
        assert(lt->lock_escalation_allowed);
    }
    lt_unlock('a');
    close_tree();
/* ******************** */
/* interleaving transactions,
   TXN A grabs 1 3 5 7 9 
   TXN B grabs 2 4 6 8 10
   make sure lock escalation fails, and that we run out of locks */
    setup_tree();
    // this should grab ten locks successfully
    for (i = 1; i < 20; i++) {
        r = lt_insert_write_no_check(i&1 ? 'a' : 'b', i);
        if (r != 0)
            break;
    }
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'a', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'b', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'c', 100);
    lt_unlock('a'); lt_unlock('b');
    close_tree();
/* ******************** */
/*
   test that escalation allowed flag goes from false->true->false
   TXN A grabs 1 3 5 7 9 
   TXN B grabs 2 4 6 8 10
   try to grab another lock, fail, lock escalation should be disabled
   txn B gets freed
   lock escalation should be reenabled
   txn C grabs 60,70,80,90,100
   lock escalation should work
*/
    setup_tree();
    assert(lt->lock_escalation_allowed);
    // this should grab ten locks successfully
    for (i = 1; i < 20; i++) {
        r = lt_insert_write_no_check(i&1 ? 'a' : 'b', i);
        if (r != 0)
            break;
    }
    assert(lt->lock_escalation_allowed);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'a', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'b', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'c', 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'a', 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'b', 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'c', 100, 100);
    lt_unlock('b');
    assert(lt->lock_escalation_allowed);
    for (i = 50; i < 1000; i++) {
        lt_insert_write(0, 'c', i);
        assert(lt->lock_escalation_allowed);
    }
    lt_unlock('a'); lt_unlock('c');
    close_tree();
/* ******************** */
/*
   txn A grabs 0,1,2,...,8  (9 locks)
   txn B grabs read lock [5,7]
   txn C attempts to grab lock, escalation, and lock grab, should fail
   lock
*/
    setup_tree();
    assert(lt->lock_escalation_allowed);
    // this should grab ten locks successfully
    for (i = 0; i < 10; i ++) { 
        if (i == 2 || i == 5) { continue; }
        lt_insert_write(0, 'a', i);
    }
    lt_insert_read (0, 'b', 5, 5);
    lt_insert_read (0, 'b', 2, 2);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'a', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'b', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'c', 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'a', 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'b', 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'c', 100, 100);
    lt_unlock('b');
    assert(lt->lock_escalation_allowed);
    for (i = 50; i < 1000; i++) {
        lt_insert_write(0, 'c', i);
        assert(lt->lock_escalation_allowed);
    }
    lt_unlock('a'); lt_unlock('c');
    close_tree();
/* ******************** */
#if 0 //Only use when messy transactions are enabled.
/*
   txn A grabs 0,1,2,...,8  (9 locks)
   txn B grabs read lock [5,7]
   txn C attempts to grab lock, escalation, and lock grab, should fail
   lock
*/
    setup_tree();
    assert(lt->lock_escalation_allowed);
    // this should grab ten locks successfully
    for (i = 0; i < 7; i++) { 
        lt_insert_write(0, 'a', i);
    }
    lt_insert_read (0, 'b', 5, 6);
    lt_insert_read (0, 'b', 2, 3);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'a', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'b', 100);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'c', 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'a', 100, 100, 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'b', 100, 100, 100, 100);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'c', 100, 100, 100, 100);
    lt_unlock('b');
    assert(lt->lock_escalation_allowed);
    for (i = 50; i < 1000; i++) {
        lt_insert_write(0, 'c', i);
        assert(lt->lock_escalation_allowed);
    }
    close_tree();
#endif
/* ******************** */
/* escalate on read lock, */
    setup_tree();
    for (i = 0; i < 10; i++) {
        lt_insert_write(0, 'a', i);
    }
    lt_insert_read(0, 'a', 10, 10);
    lt_unlock('a');
    close_tree();
/* ******************** */
/* escalate on read lock of different transaction. */
    setup_tree();
    for (i = 0; i < 10; i++) {
        lt_insert_write(0, 'a', i);
    }
    lt_insert_read(0, 'b', 10, 10);
    lt_unlock('a'); lt_unlock('b');
    close_tree();
/* ******************** */
/* txn A grabs write lock 0,9
   txn A grabs read lock 1,2,3,4,5,6,7,8
   txn B grabs write lock 11, 12, should succeed */
    setup_tree();
    for (i = 1; i < 9; i++) {
        lt_insert_read(0, 'a', i, i);
    }
    lt_insert_write(0, 'a', 0);
    lt_insert_write(0, 'a', 9);
    for (i = 50; i < 1000; i++) {
        lt_insert_write(0, 'b', i);
        assert(lt->lock_escalation_allowed);
    }
    lt_unlock('a'); lt_unlock('b');
    close_tree();
/* ******************** */
/* [1-A-5]   [10-B-15]   [20-A-25]  BORDER WRITE
    [2B]  [6C] [12A]       [22A]    READ LOCKS
    check that only last borderwrite range is escalated */
    setup_tree();
    lt_insert_write(0, 'a', 1);
    lt_insert_write(0, 'a', 5);
    lt_insert_write(0, 'b', 10);
    lt_insert_write(0, 'b', 15);
    lt_insert_write(0, 'a', 20);
    lt_insert_write(0, 'a', 23);
    lt_insert_write(0, 'a', 25);

    lt_insert_read(0, 'b', 2, 2);
    lt_insert_read(0, 'a', 12, 12);
    lt_insert_read(0, 'a', 22, 22);
    
    lt_insert_read(0, 'a', 100, 100);

    lt_insert_write(DB_LOCK_NOTGRANTED, 'b', 24);
    lt_insert_write(0, 'a', 14);
    lt_insert_write(0, 'b', 4);
    lt_unlock('a'); lt_unlock('b'); 
    close_tree();
/* ******************** */
/* Test read lock escalation, no writes. */
    setup_tree();
    assert(lt->lock_escalation_allowed);
    for (i = 0; i < 1000; i ++) { 
        lt_insert_read (0, 'b', i, i);
    }
    lt_unlock('b');
    close_tree();
/* ******************** */
/* Test read lock escalation, writes of same kind. */
    setup_tree();
    assert(lt->lock_escalation_allowed);
    lt_insert_write(0, 'b', 5);
    lt_insert_write(0, 'b', 10);
    for (i = 0; i < 1000; i ++) {   
        lt_insert_read (0, 'b', i, i);
    }
    lt_unlock('b');
    close_tree();
/* ******************** */
/* Test read lock escalation, writes of other kind. */
    setup_tree();
    assert(lt->lock_escalation_allowed);
    lt_insert_write(0, 'a', 0);
    lt_insert_write(0, 'b', 5);
    lt_insert_write(0, 'a', 7);
    lt_insert_write(0, 'c', 10);
    lt_insert_write(0, 'a', 13);
    for (i = 0; i < 1000; i ++) {  
        if (i % 5 == 0) { continue; }
        lt_insert_read (0, 'a', i, i);
    }
    lt_unlock('a'); lt_unlock('b'); lt_unlock('c');
    close_tree();
/* ******************** */
/*
   txn A grabs 0,1,2,...,8  (9 locks) (all numbers * 10)
   txn B grabs read lock [5,7] but grabs many there
   txn C attempts to grab lock, escalation, and lock grab, should fail
   lock
*/
#if 0
    setup_tree();
    assert(lt->lock_escalation_allowed);
    // this should grab ten locks successfully
    for (i = 0; i < 9; i ++) { 
        if (i == 2 || i == 5) { continue; }
        lt_insert_write(0, 'a', i*10);
    }
    for (i = 0; i < 10; i++) {
        lt_insert_read (0, 'b', 50+i, 50+i);
    }
    lt_insert_write(0, 'a', 9*10);
    lt_insert_read (0, 'b', 20, 20);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'a', 1000);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'b', 1000);
    lt_insert_write(TOKUDB_OUT_OF_LOCKS, 'c', 1000);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'a', 1000, 1000);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'b', 1000, 1000);
    lt_insert_read(TOKUDB_OUT_OF_LOCKS, 'c', 1000, 1000);
    lt_unlock('b');
    assert(lt->lock_escalation_allowed);
    for (i = 100; i < 1000; i++) {
        lt_insert_write(0, 'c', i);
        assert(lt->lock_escalation_allowed);
    }
    close_tree();
#endif
/* ******************** */
}

static void init_test(void) {
    unsigned i;
    for (i = 0; i < sizeof(nums)/sizeof(nums[0]); i++) nums[i] = i;

    buflen = 64;
    buf = (toku_range*) toku_malloc(buflen*sizeof(toku_range));
}

static void close_test(void) {
    toku_free(buf);
}

int main(int argc, const char *argv[]) {
    parse_args(argc, argv);

    init_test();

    run_escalation_test();

    close_test();

    return 0;
}