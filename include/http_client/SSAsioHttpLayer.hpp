#ifndef SSAsioHttpLayer_h__
#define SSAsioHttpLayer_h__

#include <iostream>

#include <sstream>
#include <algorithm>
#include <list>
#include <deque>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>

#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/atomic.hpp>

#include <boost/thread.hpp> 
#include <boost/noncopyable.hpp> 
#include <boost/function.hpp> 
#include <boost/enable_shared_from_this.hpp>

#include "client/SSAsioHttpClient.hpp"

#include "../test_piwik/LogDef.hpp"

using namespace boost;
using boost::asio::ip::tcp;

namespace util{
    // Http层
    class AsioHttpLayer{
        // user information
        struct UserInfo 
        {
            // id
            int request_id;
            // user callback
            SSHttp_Callback user_cb;

            UserInfo()
                : request_id(0)
            {

            }
        };

    public:
        static AsioHttpLayer& Inst()
        {
            static AsioHttpLayer ar;
            return ar;
        }

        bool Start(unsigned int threadnum = 2)
        {
            thread_num_ = threadnum;
            piwikLogDetail("Info") << "AsioHttpLayer Start begin..." << thread_num_ << "\n";
			
            bstop_ = false;
            
            for (size_t i = 0; i < thread_num_; i++)
            {
                thread_vec_.push_back(boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&AsioHttpLayer::OnRun, this))));
            }
            /*
            for (size_t i = 0; i < thread_num_; i++)
            {
                thread_vec_.push_back(
                    boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&boost::asio::io_service::run, &io_service_))));
            }*/

            piwikLogDetail("Info") << "AsioHttpLayer Start end..." << "\n";
			
            return thread_vec_.size() > 0;
        }

        void Stop(bool needwait = true)
        {
            piwikLogDetail("Info") << "AsioHttpLayer Stop begin..." << "\n";

            bstop_ = true;
            timer_.cancel();

            io_service_.stop();
            
            if (needwait)
            {
                for (auto it = thread_vec_.cbegin(); it != thread_vec_.cend(); ++it)
                {
                    (*it)->join();
                }
            }

            piwikLogDetail("Info") << "AsioHttpLayer Stop end..." << "\n";
        }

        int AddRequest(const HttpRequest& request, SSHttp_Callback cb)
        {
            piwikLogDetail("Info") << "AsioHttpLayer add request..." << "\n";
            if (bstop_)
            {
                piwikLogDetail("Err") << "AsioHttpLayer is Stopped..." << "\n";
                return -1;
            }

            int requestid = ++increase_id_;

            HttpRequest_Ptr request_ptr = HttpRequest_Ptr(new HttpRequest(request));

            _AddUser(requestid, cb);
            _AddRequest(requestid, request_ptr);

            if (_CountClient() < thread_num_)
                _RestartTimer(100);

            return requestid;
        }

        void CancelRequest(int resuestid)
        {
            piwikLogDetail("Info") << "AsioHttpLayer cancel request..." << resuestid << "\n";

            // 删除用户数据信息
            _CancelUser(resuestid);

            // 删除等待队列中
            _CancelRequest(resuestid);
            
            // 取消操作
            _CancelClient(resuestid);
        }

    public:
        boost::asio::io_service& GetIoService(){
            return io_service_;
        }

    protected:
        AsioHttpLayer()
            : thread_num_(1)
            , bstop_(false)
            , timer_(io_service_)
            , io_work_(io_service_)
            , increase_id_(0)
        {

        }

        ~AsioHttpLayer()
        {
            _CancelUserAll();
            _CancelRequestAll();
            _RemoveClientAll();
        }

        void OnRun()
        {
            piwikLogDetail("Info") << "AsioHttpLayer run begin..." << "\n";

            try
            {
                //timer_.expires_at(boost::get_system_time() + boost::posix_time::seconds(10 * 1000));
                //timer_.async_wait(boost::bind(&AsioHttpLayer::OnTimer, this, boost::asio::placeholders::error));

                //boost::asio::io_service::work w(io_service_);
                io_service_.run();
            }
            catch (std::exception& e)
            {
                piwikLogDetail("Error") << "Exception: " << e.what() << "\n";
            }

            piwikLogDetail("Info") << "AsioHttpLayer run end..." << "\n";
        }

        void OnTimer(const boost::system::error_code& err)
        {
            piwikLogDetail("Info") << "OnTimer begin" << "\n";

            // end or cancel
            if (bstop_ || err)
            {
                piwikLogDetail("Info") << "Timer Stopped or err..." << bstop_  << "--" << err << "\n";
                return;
            }

            size_t clt_count = _CountClient();
            for (size_t i = clt_count; i <= thread_num_; i++)
            {
                // get first item
                HttpRequest_Ptr ptr;
                int requestid = _PopFrontRequest(ptr);

                // dispatch this item
                if (requestid == 0 || ptr == nullptr)
                {
                    // no more task
                    std::cout << "no http request" << std::endl;
                    break;
                }

                // build&dispatch a task
                util::AsioHttpClient_Ptr c = _BuildTask(requestid, ptr);
                _AddClient(requestid, c);
                c->Start();
            }

            piwikLogDetail("Info") << "waiting size:" << _CountReqeust() << "\n";
            piwikLogDetail("Info") << "client size:" << _CountClient() << "\n";
            piwikLogDetail("Info") << "user size:" << _CountUser() << "\n";

            piwikLogDetail("Info") << "OnTimer end" << "\n";
        }

        void OnHttpCallback(HttpRequest_Ptr request_ptr, int requestid, const util::HttpResponse& response)
        {
            piwikLogDetail("Info") << "callback..." << requestid << "-" << response.http_status << "-" << response.errmsg << "\n";

            _CallbackUser(request_ptr, requestid, response);

            _RemoveClient(requestid);
            
            _RestartTimer(100);
        }
        
    private:
        inline AsioHttpClient_Ptr _BuildTask(int requestid, HttpRequest_Ptr request_ptr)
        {
            piwikLogDetail("Info") << "_BuildTask..." << "\n";

            AsioHttpClient_Ptr c(
                new AsioHttpClient(
                io_service_,
                request_ptr,
                requestid,
                boost::bind(&AsioHttpLayer::OnHttpCallback, this, _1, _2, _3))
                );
            
            return c;
        }

        inline void _RestartTimer(int timer_msecond)
        {
            piwikLogDetail("Info") << "\nRestartTimer..." << "\n\n";
            timer_.cancel();

            timer_.expires_at(boost::get_system_time() + boost::posix_time::milliseconds(timer_msecond));
            timer_.async_wait(boost::bind(&AsioHttpLayer::OnTimer, this, boost::asio::placeholders::error));
        }

    private:
        // thread num
        unsigned int thread_num_;

        // status
        volatile bool bstop_;

        // io service
        boost::asio::io_service io_service_;
        boost::asio::io_service::work io_work_;

        // io service thread
        std::vector<boost::shared_ptr<boost::thread>> thread_vec_;

        // timer
        boost::asio::deadline_timer timer_;

        // users
        boost::mutex mutex_user_;
        std::map<int, UserInfo> users_;
        // [[
        void _AddUser(int requestid, SSHttp_Callback cb)
        {
            UserInfo ui;
            ui.request_id = requestid;
            ui.user_cb = cb;

            boost::unique_lock<boost::mutex> lock(mutex_user_);
            users_[requestid] = ui;
        }

        void _CancelUser(int requestid)
        {
            boost::unique_lock<boost::mutex> lock(mutex_user_);
            auto it = users_.find(requestid);
            if (it != users_.end())
            {
                users_.erase(it);
            }
        }

        void _CallbackUser(HttpRequest_Ptr request_ptr, int requestid, const util::HttpResponse& response)
        {
            boost::unique_lock<boost::mutex> lock(mutex_user_);
            auto it = users_.find(requestid);
            if (it != users_.end())
            {
                UserInfo& ui = it->second;
                if (!ui.user_cb.empty())
                {
                    ui.user_cb(*request_ptr, requestid, response);
                }

                users_.erase(it);
            }
        }

        void _CancelUserAll()
        {
            boost::unique_lock<boost::mutex> lock(mutex_user_);
            users_.clear();
        }       

        size_t _CountUser()
        {
            boost::unique_lock<boost::mutex> lock(mutex_user_);
            return users_.size();
        }
        // ]]

        // request
        boost::mutex mutex_request_;
        std::unordered_map<int, HttpRequest_Ptr> waiting_request_;
        // [[
        void _AddRequest(int requestid, HttpRequest_Ptr ptr)
        {
            boost::unique_lock<boost::mutex> lock(mutex_request_);
            waiting_request_[requestid] = ptr;
        }
        
        int _PopFrontRequest(HttpRequest_Ptr& reqptr)
        {
            int rid = 0;
            boost::unique_lock<boost::mutex> lock(mutex_request_);
            if (waiting_request_.size() > 0)
            {
                auto it = waiting_request_.begin();

                rid = it->first;
                reqptr = it->second;
                waiting_request_.erase(it);
            }
            return rid;
        }
        
        void _CancelRequest(int requestid)
        {
            boost::unique_lock<boost::mutex> lock(mutex_request_);
            auto it = waiting_request_.find(requestid);
            if (it != waiting_request_.end())
            {
                waiting_request_.erase(it);
            }
        }

        void _CancelRequestAll()
        {
            boost::unique_lock<boost::mutex> lock(mutex_request_);
            waiting_request_.clear();
        }

        size_t _CountReqeust()
        {
            boost::unique_lock<boost::mutex> lock(mutex_request_);
            return waiting_request_.size();
        }
        // ]]

        // http client
        boost::mutex mutex_client_;
        std::map<int, AsioHttpClient_Ptr> http_clients_;
        // [[
        void _AddClient(int requestid, AsioHttpClient_Ptr c)
        {
            boost::unique_lock<boost::mutex> lock(mutex_client_);
            http_clients_.insert(std::make_pair(requestid, c));
        }
        
        void _RemoveClient(int requestid)
        {
            boost::unique_lock<boost::mutex> lock(mutex_client_);
            auto it = http_clients_.find(requestid);
            if (it != http_clients_.end())
            {
                http_clients_.erase(it);
            }
        }

        void _RemoveClientAll()
        {
            boost::unique_lock<boost::mutex> lock(mutex_client_);
            http_clients_.clear();
        }

        void _CancelClient(int requestid)
        {
            boost::unique_lock<boost::mutex> lock(mutex_client_);
            auto it = http_clients_.find(requestid);
            if (it != http_clients_.end())
            {
                it->second->Cancel();
            }
        }

        size_t _CountClient()
        {
            boost::unique_lock<boost::mutex> lock(mutex_client_);
            return http_clients_.size();
        }
        // ]]

        // ++id from 0
        boost::atomic_int increase_id_;
    };

} // namespace util

#endif // SSAsioHttpLayer_h__
