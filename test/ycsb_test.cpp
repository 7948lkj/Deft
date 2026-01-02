#include "Timer.h"
#include "Tree.h"
#include "zipf.h"

#include <city.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <random>
#include "Tree.h"
#include "Timer.h"
#include <city.h>

#include <stdlib.h>
#include <thread>
#include <time.h>
#include <vector>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>

#ifdef LONG_TEST_EPOCH
  #define TEST_EPOCH 40
  #define TIME_INTERVAL 1
#else
#ifdef SHORT_TEST_EPOCH
  #define TEST_EPOCH 5
  #define TIME_INTERVAL 0.2
#else
#ifdef MIDDLE_TEST_EPOCH
  #define TEST_EPOCH 10
  #define TIME_INTERVAL 1
#else
  #define TEST_EPOCH 10
  #define TIME_INTERVAL 0.5
#endif
#endif
#endif

#define MAX_THREAD_REQUEST 10000000
#define LOAD_HEARTBEAT 100000
#define USE_CORO
#define EPOCH_LAT_TEST
#define LOADER_NUM 8 // [CONFIG] 8

// extern double cache_miss[MAX_APP_THREAD];
// extern double cache_hit[MAX_APP_THREAD];
// extern uint64_t lock_fail[MAX_APP_THREAD];
// extern uint64_t write_handover_num[MAX_APP_THREAD];
// extern uint64_t try_write_op[MAX_APP_THREAD];
// extern uint64_t read_handover_num[MAX_APP_THREAD];
// extern uint64_t try_read_op[MAX_APP_THREAD];
// extern uint64_t read_leaf_retry[MAX_APP_THREAD];
// extern uint64_t leaf_cache_invalid[MAX_APP_THREAD];
// extern uint64_t leaf_read_sibling[MAX_APP_THREAD];
// extern uint64_t try_speculative_read[MAX_APP_THREAD];
// extern uint64_t correct_speculative_read[MAX_APP_THREAD];
// extern uint64_t try_read_leaf[MAX_APP_THREAD];
// extern uint64_t read_two_segments[MAX_APP_THREAD];
// extern uint64_t try_read_hopscotch[MAX_APP_THREAD];
// extern uint64_t try_insert_op[MAX_APP_THREAD];
// extern uint64_t split_node[MAX_APP_THREAD];
// extern uint64_t try_write_segment[MAX_APP_THREAD];
// extern uint64_t write_two_segments[MAX_APP_THREAD];
// extern double load_factor_sum[MAX_APP_THREAD];
// extern uint64_t split_hopscotch[MAX_APP_THREAD];

int kThreadCount;
int kNodeCount;
int kCoroCnt = 8;
bool kIsScan;
#ifdef USE_CORO
bool kUseCoro = true;
#else
bool kUseCoro = false;
#endif

std::string ycsb_load_path;
std::string ycsb_trans_path;
int fix_range_size = -1;
// turn it on if want to eliminate impact of write conflicts
bool rm_write_conflict = false;


std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][MAX_CORO_NUM] = {0};

extern volatile bool need_stop;
extern volatile bool need_clear[MAX_APP_THREAD];
extern uint64_t latency[MAX_APP_THREAD][MAX_CORO_NUM][LATENCY_WINDOWS];
uint64_t latency_th_all[LATENCY_WINDOWS];

std::default_random_engine e;
std::uniform_int_distribution<Value> randval(0, UINT64_MAX);

Tree *tree;
DSMClient *dsm_client;


inline uint64_t key_hash(const Key &k) {
  return CityHash64((char *)&k, sizeof(k));
}


class RequsetGenBench : public RequstGen {
public:
  RequsetGenBench(DSMClient* dsm_client, Request* req, int req_num, int coro_id, int coro_cnt) :
                  dsm_client(dsm_client), req(req), req_num(req_num), coro_id(coro_id), coro_cnt(coro_cnt) {
    local_thread_id = dsm_client->get_my_thread_id();
    cur = coro_id;
    epoch_id = 0;
    // extra_k = MAX_KEY_SPACE_SIZE + kThreadCount * kCoroCnt * dsm_client->getMyNodeID() + local_thread_id * kCoroCnt + coro_id;
    flag = false;
  }

  Request next() override {
    cur = (cur + coro_cnt) % req_num;
    if (req[cur].req_type == INSERT) {
      if (cur + coro_cnt >= req_num) {
        // need_stop = true;
        ++ epoch_id;
        flag = true;
      }
      // if (flag) {
      //   req[cur].k = int2key(extra_k);
      //   extra_k += kThreadCount * kCoroCnt * dsm->getClusterSize();
      // }
    }
    tp[local_thread_id][coro_id]++;
    req[cur].v = randval(e);
    return req[cur];
  }

private:
  DSMClient *dsm_client;
  Request* req;
  int req_num;
  int coro_id;
  int coro_cnt;
  int local_thread_id;
  int cur;
  uint8_t epoch_id;
  uint64_t extra_k;
  bool flag;
};


RequstGen *gen_func(DSMClient *dsm_client, Request* req, int req_num, int coro_id, int coro_cnt) {
  return new RequsetGenBench(dsm_client, req, req_num, coro_id, coro_cnt);
}


void work_func(Tree *tree, const Request& r, CoroContext *ctx) {
  if (r.req_type == SEARCH) {
    Value v;
    tree->search(r.k, v, ctx);
  }
  else if (r.req_type == INSERT) {
    tree->insert(r.k, r.v, ctx);
  }
  else if (r.req_type == UPDATE) {
    tree->insert(r.k, r.v, ctx);
  }
  else {
    std::map<Key, Value> ret;
    Value buffer[r.range_size];
    tree->range_query(r.k, r.k + r.range_size, buffer, r.range_size);
  }
}


Timer bench_timer;
std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool ready{false};


void thread_load(int id) {
  // use LOADER_NUM threads to load ycsb
  uint64_t loader_id = std::min(kThreadCount, LOADER_NUM) * dsm_client->get_my_client_id() + id;

  printf("I am loader %lu\n", loader_id);

  // 1. insert ycsb_load
  std::string op;
  std::ifstream load_in(ycsb_load_path + std::to_string(loader_id));
  if (!load_in.is_open()) {
    printf("Error opening load file\n");
    assert(false);
  }
  Key k;
  int cnt = 0;
  uint64_t int_k;
  while (load_in >> op >> int_k) {
    // k = int2key(int_k);
    assert(op == "INSERT");
    tree->insert(k, k + 23);
    if (++ cnt % LOAD_HEARTBEAT == 0) {
      printf("thread %lu: %d load entries loaded.\n", loader_id, cnt);
    }
  }
  printf("loader %lu load finish\n", loader_id);
}


void thread_run(int id) {
  bindCore(id * 2 + 1);  // bind to CPUs in NUMA that close to mlx5_2

  dsm_client->RegisterThread();
  uint64_t my_id = kThreadCount * dsm_client->get_my_client_id() + id;

  printf("I am %lu\n", my_id);

  if (id == 0) {
    bench_timer.begin();
  }

  // 1. insert ycsb_load
  if (id < std::min(kThreadCount, LOADER_NUM)) {
    thread_load(id);
  }

  // 2. load ycsb_trans
  Request* req = new Request[MAX_THREAD_REQUEST];
  int req_num = 0;
  std::ifstream trans_in(ycsb_trans_path + std::to_string(my_id));
  if (!trans_in.is_open()) {
    printf("Error opening trans file\n");
    assert(false);
  }
  std::string op;
  int cnt = 0;
  int range_size = 0;
  uint64_t int_k;
  while(trans_in >> op >> int_k) {
    if (op == "SCAN") trans_in >> range_size;
    else range_size = 0;
    Request r;
    r.req_type = (op == "READ"  ? SEARCH : (
                  op == "INSERT"? INSERT : (
                  op == "UPDATE"? UPDATE : SCAN
    )));
    r.range_size = fix_range_size >= 0 ? fix_range_size : range_size;
    // r.k = int2key(int_k);
    // if (rm_write_conflict) {
    //   if (r.req_type == UPDATE || r.req_type == INSERT) {
    //     uint64_t all_thread_num = kThreadCount * dsm->getClusterSize();
    //     r.k = dsm->getNoComflictKey(key_hash(r.k), my_id, all_thread_num);
    //   }
    // }
    r.k = int_k;
    r.v = r.k + 23;
    req[req_num ++] = r;
    if (++ cnt % LOAD_HEARTBEAT == 0) {
      printf("thread %d: %d trans entries loaded.\n", id, cnt);
    }
  }

  warmup_cnt.fetch_add(1);

  if (id == 0) {
    while (warmup_cnt.load() != kThreadCount)
      ;
    printf("node %d finish\n", dsm_client->get_my_client_id());
    dsm_client->Barrier("warm_finish");

    uint64_t ns = bench_timer.end();
    printf("warmup time %lds\n", ns / 1000 / 1000 / 1000);

    ready = true;
    warmup_cnt.store(-1);
  }
  while (warmup_cnt.load() != -1)
    ;

  // 3. start ycsb test
  if (!kIsScan && kUseCoro) {
    tree->run_coroutine_ycsb(gen_func, work_func, kCoroCnt, req, req_num, id);
  }
  else {
    /// without coro
    Timer timer;
    auto gen = new RequsetGenBench(dsm_client, req, req_num, 0, 0);
    auto thread_id = dsm_client->get_my_thread_id();

    while (!need_stop) {
      auto r = gen->next();

      timer.begin();
      work_func(tree, r, nullptr);
      auto us_10 = timer.end() / 100;

      if (us_10 >= LATENCY_WINDOWS) {
        us_10 = LATENCY_WINDOWS - 1;
      }
      latency[thread_id][0][us_10]++;
    }
  }
}

void parse_args(int argc, char *argv[]) {
  if (argc != 6 && argc != 7) {
    printf("Usage: ./ycsb_test kNodeCount kThreadCount kCoroCnt workload_type[randint] workload_idx[a/b/c/d/e] [fix_range_size/rm_write_conflict]\n");
    exit(-1);
  }

  kNodeCount = atoi(argv[1]);
  kThreadCount = atoi(argv[2]);
  kCoroCnt = atoi(argv[3]);
  assert(std::string(argv[4]) == "randint");  // currently only int workloads is tested
  kIsScan = (std::string(argv[5]) == "e");

  std::string workload_dir;
  std::ifstream workloads_dir_in("../workloads.conf");
  if (!workloads_dir_in.is_open()) {
    printf("Error opening workloads.conf\n");
    assert(false);
  }
  workloads_dir_in >> workload_dir;
  ycsb_load_path = workload_dir + "/load_" + std::string(argv[4]) + "_workload" + std::string(argv[5]);
  ycsb_trans_path = workload_dir + "/txn_" + std::string(argv[4]) + "_workload" + std::string(argv[5]);
  if (argc == 7) {
    if(kIsScan) fix_range_size = atoi(argv[6]);
    else rm_write_conflict = (atoi(argv[6]) != 0);
  }

  printf("kNodeCount %d, kThreadCount %d, kCoroCnt %d\n", kNodeCount, kThreadCount, kCoroCnt);
  printf("ycsb_load: %s\n", ycsb_load_path.c_str());
  printf("ycsb_trans: %s\n", ycsb_trans_path.c_str());
  if (argc == 7) {
    if(kIsScan) printf("fix_range_size: %d\n", fix_range_size);
    else printf("rm_write_conflict: %s\n", rm_write_conflict ? "true" : "false");
  }
}

void save_latency(int epoch_id) {
  // sum up local latency cnt
  for (int i = 0; i < LATENCY_WINDOWS; ++i) {
    latency_th_all[i] = 0;
    for (int k = 0; k < MAX_APP_THREAD; ++k)
      for (int j = 0; j < MAX_CORO_NUM; ++j) {
        latency_th_all[i] += latency[k][j][i];
    }
  }
  // store in file
  std::ofstream f_out("../us_lat/epoch_" + std::to_string(epoch_id) + ".lat");
  f_out << std::setiosflags(std::ios::fixed) << std::setprecision(1);
  if (f_out.is_open()) {
    for (int i = 0; i < LATENCY_WINDOWS; ++i) {
      f_out << i / 10.0 << "\t" << latency_th_all[i] << std::endl;
    }
    f_out.close();
  }
  else {
    printf("Fail to write file!\n");
    assert(false);
  }
  memset(latency, 0, sizeof(uint64_t) * MAX_APP_THREAD * MAX_CORO_NUM * LATENCY_WINDOWS);
}

int main(int argc, char *argv[]) {

  parse_args(argc, argv);

  DSMConfig config;
  config.num_server = 1;
  config.num_client = kNodeCount;
  // assert(kNodeCount >= MEMORY_NODE_NUM);
  // config.machineNR = kNodeCount;
  // config.threadNR = kThreadCount;
  dsm_client = DSMClient::GetInstance(config);
  bindCore(kThreadCount * 2 + 1);
// #ifdef ENABLE_CACHE_EVICTION
//   dsm_client->loadKeySpace(ycsb_load_path, false);
// #else
//   if (rm_write_conflict) {
//     dsm_client->loadKeySpace(ycsb_load_path, false);
//   }
// #endif
  dsm_client->RegisterThread();
  tree = new Tree(dsm_client);
  dsm_client->Barrier("benchmark");

  for (int i = 0; i < kThreadCount; i ++) {
    th[i] = std::thread(thread_run, i);
  }

  while (!ready.load())
    ;
  timespec s, e;
  uint64_t pre_tp = 0;
  int count = 0;

  clock_gettime(CLOCK_REALTIME, &s);
  while(!need_stop) {

    sleep(TIME_INTERVAL);
    clock_gettime(CLOCK_REALTIME, &e);
    int microseconds = (e.tv_sec - s.tv_sec) * 1000000 +
                       (double)(e.tv_nsec - s.tv_nsec) / 1000;

    uint64_t all_tp = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i) {
      for (int j = 0; j < kCoroCnt; ++j)
        all_tp += tp[i][j];
    }
    clock_gettime(CLOCK_REALTIME, &s);

    uint64_t cap = all_tp - pre_tp;
    pre_tp = all_tp;
    save_latency(++ count);

    double per_node_tp = cap * 1.0 / microseconds;
    uint64_t cluster_tp = dsm_client->Sum((uint64_t)(per_node_tp * 1000));  // only node 0 return the sum

    printf("%d, throughput %.4f\n", dsm_client->get_my_client_id(), per_node_tp);

    if (dsm_client->get_my_client_id() == 0) {
      printf("epoch %d passed!\n", count);
      printf("cluster throughput %.3f Mops\n", cluster_tp / 1000.0);
      // printf("cache hit rate: %.4lf\n", hit * 1.0 / all);
      // printf("avg. lock/cas fail cnt: %.4lf\n", lock_fail_cnt * 1.0 / try_write_op_cnt);
      // printf("write combining rate: %.4lf\n", write_handover_cnt * 1.0 / try_write_op_cnt);
      // printf("read delegation rate: %.4lf\n", read_handover_cnt * 1.0 / try_read_op_cnt);
      // printf("read leaf retry rate: %.4lf\n", read_leaf_retry_cnt * 1.0 / try_read_leaf_cnt);
      // printf("read invalid leaf rate: %.4lf\n", leaf_cache_invalid_cnt * 1.0 / try_read_leaf_cnt);
      // printf("read sibling leaf rate: %.4lf\n", leaf_read_sibling_cnt * 1.0 / try_read_leaf_cnt);
      // printf("speculative read rate: %.4lf\n", try_speculative_read_cnt * 1.0 / try_read_leaf_cnt);
      // printf("correct ratio of speculative read: %.4lf\n", correct_speculative_read_cnt * 1.0 / try_speculative_read_cnt);
      // printf("read two hopscotch-segments rate: %.4lf\n", read_two_segments_cnt * 1.0 / try_read_hopscotch_cnt);
      // printf("write two hopscotch-segments rate: %.4lf\n", write_two_segments_cnt * 1.0 / try_write_segment_cnt);
      // printf("node split rate: %.4lf\n", split_node_cnt * 1.0 / try_insert_op_cnt);
      // printf("avg. leaf load factor: %.4lf\n", load_factor_sum_all * 1.0 / split_hopscotch_cnt);
      printf("\n");
    }
    if (count >= TEST_EPOCH) {
      need_stop = true;
    }
  }
#ifndef EPOCH_LAT_TEST
  save_latency(1);
#endif
  for (int i = 0; i < kThreadCount; i++) {
    th[i].join();
    printf("Thread %d joined.\n", i);
  }
  // tree->statistics();
  printf("[END]\n");
  dsm_client->Barrier("fin");
  
  if (dsm_client->get_my_client_id() == 0) {
    RawMessage m;
    m.type = RpcType ::TERMINATE;
    for (uint32_t i = 0; i < config.num_server; ++i) {
      dsm_client->RpcCallDir(m, i);
    }
  }

  return 0;
}
