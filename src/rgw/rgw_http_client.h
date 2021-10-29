// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#ifndef CEPH_RGW_HTTP_CLIENT_H
#define CEPH_RGW_HTTP_CLIENT_H

#include "common/async/yield_context.h"
#include "common/RWLock.h"
#include "common/Cond.h"
#include "rgw_common.h"
#include "rgw_string.h"
#include "rgw_http_client_types.h"

#include <atomic>

using param_pair_t = pair<string, string>;
using param_vec_t = vector<param_pair_t>;

void rgw_http_client_init(CephContext *cct);
void rgw_http_client_cleanup();

struct rgw_http_req_data;
class RGWHTTPManager;

// 对 rgw_http_req_data 的进一步封装，client的初始化，定义了此client的一系列读写回调函数。
class RGWHTTPClient : public RGWIOProvider
{
  friend class RGWHTTPManager;

  bufferlist send_bl;       // 发送数据的缓存
  bufferlist::iterator send_iter;
  bool has_send_len;
  long http_status;
  bool send_data_hint{false};
  size_t receive_pause_skip{0}; /* 下次调用receive_data，跳过的字节数
                                   how many bytes to skip next time receive_data is called
                                   due to being paused */

  void *user_info{nullptr};

  rgw_http_req_data *req_data;  // 指向 rgw_http_req_data 

  bool verify_ssl; // Do not validate self signed certificates, default to false

  std::atomic<unsigned> stopped { 0 };


protected:
  CephContext *cct;

  /*
  ** 定义访问的url，http方法
  ** http头部，及超时设置
  */
  string method;
  string url;

  size_t send_len{0};

  param_vec_t headers;

  long  req_timeout{0L};

  RGWHTTPManager *get_manager();
  /*
  ** client的设置：
  ** 获取CURL* 句柄： curl_easy_init
  ** 设置CURL* 句柄参数： curl_easy_setopt
  ** headers的设置等内容
  */
  int init_request(rgw_http_req_data *req_data);

  
  virtual int receive_header(void *ptr, size_t len) {
    return 0;
  }
  virtual int receive_data(void *ptr, size_t len, bool *pause) {
    return 0;
  }

  virtual int send_data(void *ptr, size_t len, bool *pause=nullptr) {
    return 0;
  }

  /* Callbacks for libcurl. */
  static size_t receive_http_header(void *ptr,
                                    size_t size,
                                    size_t nmemb,
                                    void *_info);

  static size_t receive_http_data(void *ptr,
                                  size_t size,
                                  size_t nmemb,
                                  void *_info);

  static size_t send_http_data(void *ptr,
                               size_t size,
                               size_t nmemb,
                               void *_info);

  ceph::mutex& get_req_lock();

  /* needs to be called under req_lock() */
  void _set_write_paused(bool pause);
  void _set_read_paused(bool pause);
public:
  static const long HTTP_STATUS_NOSTATUS     = 0;
  static const long HTTP_STATUS_UNAUTHORIZED = 401;
  static const long HTTP_STATUS_NOTFOUND     = 404;

  static constexpr int HTTPCLIENT_IO_READ    = 0x1;
  static constexpr int HTTPCLIENT_IO_WRITE   = 0x2;
  static constexpr int HTTPCLIENT_IO_CONTROL = 0x4;

  virtual ~RGWHTTPClient();
  explicit RGWHTTPClient(CephContext *cct,
                         const string& _method,
                         const string& _url)
    : has_send_len(false),
      http_status(HTTP_STATUS_NOSTATUS),
      req_data(nullptr),
      verify_ssl(cct->_conf->rgw_verify_ssl),
      cct(cct),
      method(_method),
      url(_url) {
  }

  void append_header(const string& name, const string& val) {
    headers.push_back(pair<string, string>(name, val));
  }

  void set_send_length(size_t len) {
    send_len = len;
    has_send_len = true;
  }

  void set_send_data_hint(bool hint) {
    send_data_hint = hint;
  }

  long get_http_status() const {
    return http_status;
  }

  void set_http_status(long _http_status) {
    http_status = _http_status;
  }

  void set_verify_ssl(bool flag) {
    verify_ssl = flag;
  }

  // set request timeout in seconds
  // zero (default) mean that request will never timeout
  void set_req_timeout(long timeout) {
    req_timeout = timeout;
  }

  int process(optional_yield y);

  int wait(optional_yield y);
  void cancel();
  bool is_done();

  rgw_http_req_data *get_req_data() { return req_data; }

  string to_str();

  int get_req_retcode();

  void set_url(const string& _url) {
    url = _url;
  }

  void set_method(const string& _method) {
    method = _method;
  }

  void set_io_user_info(void *_user_info) override {
    user_info = _user_info;
  }

  void *get_io_user_info() override {
    return user_info;
  }
};


class RGWHTTPHeadersCollector : public RGWHTTPClient {
public:
  typedef std::string header_name_t;
  typedef std::string header_value_t;
  typedef std::set<header_name_t, ltstr_nocase> header_spec_t;

  RGWHTTPHeadersCollector(CephContext * const cct,
                          const string& method,
                          const string& url,
                          const header_spec_t &relevant_headers)
    : RGWHTTPClient(cct, method, url),
      relevant_headers(relevant_headers) {
  }

  std::map<header_name_t, header_value_t, ltstr_nocase> get_headers() const {
    return found_headers;
  }

  /* Throws std::out_of_range */
  const header_value_t& get_header_value(const header_name_t& name) const {
    return found_headers.at(name);
  }

protected:
  int receive_header(void *ptr, size_t len) override;

private:
  const std::set<header_name_t, ltstr_nocase> relevant_headers;
  std::map<header_name_t, header_value_t, ltstr_nocase> found_headers;
};


class RGWHTTPTransceiver : public RGWHTTPHeadersCollector {
  bufferlist * const read_bl;
  std::string post_data;
  size_t post_data_index;

public:
  RGWHTTPTransceiver(CephContext * const cct,
                     const string& method,
                     const string& url,
                     bufferlist * const read_bl,
                     const header_spec_t intercept_headers = {})
    : RGWHTTPHeadersCollector(cct, method, url, intercept_headers),
      read_bl(read_bl),
      post_data_index(0) {
  }

  RGWHTTPTransceiver(CephContext * const cct,
                     const string& method,
                     const string& url,
                     bufferlist * const read_bl,
                     const bool verify_ssl,
                     const header_spec_t intercept_headers = {})
    : RGWHTTPHeadersCollector(cct, method, url, intercept_headers),
      read_bl(read_bl),
      post_data_index(0) {
    set_verify_ssl(verify_ssl);
  }

  void set_post_data(const std::string& _post_data) {
    this->post_data = _post_data;
  }

protected:
  int send_data(void* ptr, size_t len, bool *pause=nullptr) override;

  int receive_data(void *ptr, size_t len, bool *pause) override {
    read_bl->append((char *)ptr, len);
    return 0;
  }
};

typedef RGWHTTPTransceiver RGWPostHTTPData;


class RGWCompletionManager;

enum RGWHTTPRequestSetState {
  SET_NOP = 0,
  SET_WRITE_PAUSED = 1,
  SET_WRITE_RESUME = 2,
  SET_READ_PAUSED  = 3,
  SET_READ_RESUME  = 4,
};

/*
通过管道进行线程间通信，如注册 client
独立线程，对所有注册的client，进行Libcurl的 perform
 */
class RGWHTTPManager {

  // client 状态
  struct set_state {
    rgw_http_req_data *req;
    int bitmask;

    set_state(rgw_http_req_data *_req, int _bitmask) : req(_req), bitmask(_bitmask) {}
  };
  CephContext *cct;
  RGWCompletionManager *completion_mgr;

  // CURLM* curl_multi_init()
  void *multi_handle;
  bool is_started = false;
  std::atomic<unsigned> going_down { 0 };
  std::atomic<unsigned> is_stopped { 0 };


  // 管理client的读写锁
  ceph::shared_mutex reqs_lock = ceph::make_shared_mutex("RGWHTTPManager::reqs_lock");

  // reqs: 需要进行http请求的client集合
  map<uint64_t, rgw_http_req_data *> reqs;

  list<rgw_http_req_data *> unregistered_reqs;

  list<set_state> reqs_change_state;
  map<uint64_t, rgw_http_req_data *> complete_reqs;
  int64_t num_reqs = 0;
  int64_t max_threaded_req = 0;
  int thread_pipe[2];       // 线程间同步

  /*
  ** 注册一个libcurl client
  ** 1. std::unique_lock rl{reqs_lock};
  ** 2. req_data->id = num_reqs;
  ** 3. req_data->registered = true;
  ** 4. reqs[num_reqs] = req_data;
  ** 5. num_reqs++;
  */
  void register_request(rgw_http_req_data *req_data);

  /*
  ** 清理request的函数
  ** 1. std::unique_lock rl{reqs_lock};
  ** 2. _complete_request(req_data);
  */
  void complete_request(rgw_http_req_data *req_data);

  /*
  ** 清理reqs中的req_data:
  ** 1. map<uint64_t, rgw_http_req_data *>::iterator iter = reqs.find(req_data->id);
  ** 2. reqs.erase(iter);
  **  {
  **    std::lock_guard l{req_data->lock};
  **    req_data->mgr = nullptr;
  **  }
  **  if (completion_mgr) {
  **    completion_mgr->complete(NULL, req_data->control_io_id, req_data->user_info);
  **  }
  ** 3. 释放一个引用计数: req_data->put();
  */
  void _complete_request(rgw_http_req_data *req_data);

  /*
  ** 注销一个libcurl client,加入 unregistered_reqs 队列中，待主线程处理:
  ** 1. std::unique_lock rl{reqs_lock};
  ** 2. if (!req_data->registered) {
  **      return false;
  **    }
  ** 3. req_data->get();
  ** 4. req_data->registered = false;
  ** 5. unregistered_reqs.push_back(req_data);
  */
  bool unregister_request(rgw_http_req_data *req_data);

  /*
  ** CURLM* 中删除一个 easy interface:
  ** unlink:
      1. std::unique_lock wl{reqs_lock};
      2. _unlink_request(req_data);
  ** _unlink_request:
  ** 从CURLM中删除
  **  1. curl_multi_remove_handle((CURLM *)multi_handle, req_data->get_easy_handle());
  ** 判断client请求是否完成:
  ** - 没有完成状态，则设置返回码: ECANCELED; _finish_request中调用req_data->finish(-ECANCELED)，通知client
  ** - 如果是完成状态，则无需在调用_finish_request，因为完成状态，说明已经正产调用过finish_request
  **  2. if (!req_data->is_done()) 
  **        _finish_request(req_data, -ECANCELED);
  */
  void _unlink_request(rgw_http_req_data *req_data);
  void unlink_request(rgw_http_req_data *req_data);

  /*
  ** 结束一个client的请求：
  ** 结束一个client请求，两种情形：
  **  1. 请求完成，调用finish_request
  **  2. 取消client请求，调用cancel,调用client->cancel: httpmanager->unregister_request
  ** finish_request(req_data,r,http_status): http_status是http返回码，status是http_status对应的本地定义状态
  **  1. req_data->finish(ret, http_status);
  **  2. complete_request(req_data);
  ** _finish_request(rgw_http_req_data *req_data, int r):
  **  1. req_data->finish(ret);
  **  2. complete_request(req_data);
  */
  void finish_request(rgw_http_req_data *req_data, int r, long http_status = -1);
  void _finish_request(rgw_http_req_data *req_data, int r);

  /*
  ** 设置req状态req->set_state >> CURLcode rc = curl_easy_pause(**curl_handle, bitmask);：
  **  ss.req->set_state(ss.bitmask)
  */
  void _set_req_state(set_state& ss);

  /*
  ** 将easy_interface绑定至multi handle:
  **  1. CURLMcode mstatus = curl_multi_add_handle((CURLM *)multi_handle, req_data->get_easy_handle());
  */
  int link_request(rgw_http_req_data *req_data);


  /*
  ** 处理一些处于待处理队列中的client, 如：unregistered_reqs/reqs_change_state
  */
  void manage_pending_requests();

  class ReqsThread : public Thread {
    RGWHTTPManager *manager;

  public:
    explicit ReqsThread(RGWHTTPManager *_m) : manager(_m) {}
    void *entry() override;
  };

  ReqsThread *reqs_thread = nullptr;


  /*
  ** RGWHTTPManager处理线程，具体逻辑见函数解析
  */
  void *reqs_thread_entry();

  /*
  ** 线程间同步：
  ** 1. write(thread_pipe[1], (void *)&buf, sizeof(buf));
  */
  int signal_thread();

public:
  /*
  ** 构造函数:
  ** 1. 初始化CURLM*
  **    multi_handle = (void *)curl_multi_init();
  */
  RGWHTTPManager(CephContext *_cct, RGWCompletionManager *completion_mgr = NULL);

  /*
  ** 析构函数,清理CURLM*：
  ** 1. stop()
  ** 2. curl_multi_cleanup((CURLM *)multi_handle)
  **
  */
  ~RGWHTTPManager();

  /*
  ** 创建 libcurl client 处理线程:
  ** 1. pipe2(thread_pipe)
  ** 2. fcntl(thread_pipe[0], F_SETFL, O_NONBLOCK); 设置非阻塞
  ** 3. is_started = true;
  **    reqs_thread = new ReqsThread(this);
  **    reqs_thread->create("http_manager");
  ** 4. reqs_thread->create() >> manager->reqs_thread_entry();
  */
  int start();

  /*
  ** 关闭libcurl client处理线程:
  ** 1. is_stopped = true
  ** 2. going_down = true
  ** 3. signal_thread()
  ** 4. reqs_thread->join()
  ** 5. delete reqs_thread
  ** 6. close(thread_pipe[0])
  **    close(thread_pipe[1])
  */
  void stop();

  /*
  ** 注册一个client:
  **  1. rgw_http_req_data *req_data = new rgw_http_req_data;
  **  2. client->init_request(req_data)
  **  3. register_request(req_data)
  **  4. link_request(req_data)
  **  5. signal_thread() 通知libcurl的线程有client的改变
  */
  int add_request(RGWHTTPClient *client);

  /*
  ** 删除一个 libcurl client:
  **  1. rgw_http_req_data *req_data = client->get_req_data()
  **  2. unregister_request(req_data)
  **  3. signal_thread() 
  */
  int remove_request(RGWHTTPClient *client);

  /*
  ** 设置一个client的状态，加入reqs_change_state队列，待处理线程处理：
  **  1. int bitmask = CURLPAUSE_CONT;
         if (req_data->write_paused) {
            bitmask |= CURLPAUSE_SEND;
         }
         if (req_data->read_paused) {
            bitmask |= CURLPAUSE_RECV;
         }
  ** 2. reqs_change_state.push_back(set_state(req_data, bitmask))
  ** 3. signal_thread()
  */
  int set_request_state(RGWHTTPClient *client, RGWHTTPRequestSetState state);
};

class RGWHTTP
{
public:
  static int send(RGWHTTPClient *req);
  static int process(RGWHTTPClient *req, optional_yield y);
};
#endif
