#include "ClothesStreaming.h"
#include <fstream>
#include "../character/CharacterLog.h"
namespace Eagle::Character {
ClothesStreaming::ClothesStreaming(std::string root,const CharacterCatalog& catalog):root_(std::move(root)),catalog_(catalog),loader_(root_,catalog_),worker_(&ClothesStreaming::Worker,this) {}
ClothesStreaming::~ClothesStreaming() {
    {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}changed_.notify_all();if(worker_.joinable()) worker_.join();
    cache_.clear(); // RW destruction stays on the owner thread; worker only reads bytes.
}
std::shared_ptr<AssetTicket> ClothesStreaming::Acquire(const std::string& id) {
    auto found=cache_.find(id);if(found!=cache_.end()) {found->second->idleSince=0;return found->second;}
    if(!catalog_.Asset(id)||cache_.size()>=256) return {};
    std::lock_guard<std::mutex> lock(mutex_);if(jobs_.size()>=64||stop_) return {};
    auto ticket=std::make_shared<AssetTicket>();cache_[id]=ticket;jobs_.push_back(id);changed_.notify_one();return ticket;
}
void ClothesStreaming::Worker() {
    for(;;) {
        std::string id;
        {std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[&]{return stop_||(!jobs_.empty()&&replies_.size()<4);});
            if(stop_) return;id=std::move(jobs_.front());jobs_.pop_front();}
        Reply reply;reply.key=id;
        try {
            const auto* def=catalog_.Asset(id);
            std::ifstream input(root_+"/"+def->file,std::ios::binary|std::ios::ate);
            if(!input||input.tellg()<1||input.tellg()>4*1024*1024) reply.error="asset missing/size limit";
            else {reply.bytes.resize(size_t(input.tellg()));input.seekg(0);if(!input.read(reinterpret_cast<char*>(reply.bytes.data()),reply.bytes.size())) reply.error="asset read failed";}
            if(reply.error.empty()) ClothesLoader::ValidateDff(reply.bytes,reply.error);
        } catch(const std::exception& e) {reply.error=e.what();reply.bytes.clear();}
        {std::lock_guard<std::mutex> lock(mutex_);if(stop_) return;replies_.push_back(std::move(reply));}
    }
}
void ClothesStreaming::Tick(uint64_t now) {
    // One native stream/upload per frame. DFF file reads run on the worker;
    // the small shared PNGs are decoded by the existing RW image wrapper.
    Reply reply;bool have=false;
    {std::lock_guard<std::mutex> lock(mutex_);if(!replies_.empty()) {reply=std::move(replies_.front());replies_.pop_front();have=true;changed_.notify_one();}}
    if(have) {
        auto found=cache_.find(reply.key);
        if(found!=cache_.end()) {
            auto& ticket=*found->second;ticket.error=reply.error;
            const size_t required=reply.bytes.size()*4;
            // Rapid browsing may leave several unused looks in the delayed
            // unload window. Reclaim those before rejecting a new selection.
            if(ticket.error.empty()&&resident_+required>64*1024*1024) {
                for(auto idle=cache_.begin();idle!=cache_.end()&&resident_+required>64*1024*1024;) {
                    if(idle!=found&&idle->second.use_count()==1&&idle->second->state!=AssetTicket::State::Reading) {
                        if(idle->second->asset) resident_-=idle->second->asset->memory;
                        idle=cache_.erase(idle);
                    }else ++idle;
                }
            }
            if(ticket.error.empty()&&resident_+required>64*1024*1024) ticket.error="64 MiB asset cache budget";
            if(ticket.error.empty()) ticket.asset=loader_.Load(*catalog_.Asset(reply.key),reply.bytes,ticket.error);
            if(ticket.asset) {ticket.state=AssetTicket::State::Ready;resident_+=ticket.asset->memory;}
            else {ticket.state=AssetTicket::State::Failed;LogLine(ANDROID_LOG_ERROR,"asset %s: %s",reply.key.c_str(),ticket.error.c_str());}
        }
    }
    for(auto it=cache_.begin();it!=cache_.end();) {
        auto& ticket=it->second;
        if(ticket.use_count()==1&&ticket->state!=AssetTicket::State::Reading) {
            if(!ticket->idleSince) ticket->idleSince=now;
            if(now-ticket->idleSince>5000) {if(ticket->asset) resident_-=ticket->asset->memory;it=cache_.erase(it);continue;}
        }else ticket->idleSince=0;
        ++it;
    }
}
}
