#include <cascade/user_defined_logic_interface.hpp>
#include <iostream>
#include <mutex>
#include <vector>
#include <opencv2/opencv.hpp>
#include <cppflow/cppflow.h>
#include "demo_udl.hpp"
#include "time_probes.hpp"

namespace derecho{
namespace cascade{

#define MY_UUID     "22b86c6e-9d92-11eb-81d0-0242ac110002"
#define MY_DESC     "The Dairy Farm DEMO: Filter UDL."

std::string get_uuid() {
    return MY_UUID;
}

std::string get_description() {
    return MY_DESC;
}

#define FILTER_THRESHOLD       (0.9)
#define IMAGE_WIDTH            (352)
#define IMAGE_HEIGHT           (240)
#define FILTER_TENSOR_BUFFER_SIZE     (IMAGE_WIDTH*IMAGE_HEIGHT*3)
#define CONF_FILTER_MODEL             "filter-model"

class DairyFarmFilterOCDPO: public OffCriticalDataPathObserver {
    std::mutex p2p_send_mutex;
    static std::mutex init_mutex;

    virtual void operator () (const std::string& key_string,
                              const uint32_t prefix_length,
                              persistent::version_t version,
                              const mutils::ByteRepresentable* const value_ptr,
                              const std::unordered_map<std::string,bool>& outputs,
                              ICascadeContext* ctxt,
                              uint32_t worker_id) override {
        // test if there is a cow in the incoming frame.
        auto* typed_ctxt = dynamic_cast<DefaultCascadeContextType*>(ctxt);
#ifdef ENABLE_EVALUATION
        if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
            global_timestamp_logger.log(TLT_FRONTEND_TRIGGERED,
                                        typed_ctxt->get_service_client_ref().get_my_id(),
                                        reinterpret_cast<const ObjectWithStringKey*>(value_ptr)->get_message_id(),
                                        get_walltime());
        }
#endif
        /* step 1: load the model */ 
        static thread_local std::unique_ptr<cppflow::model> model;
        if (!model){
            std::lock_guard lck(init_mutex);
            model = std::make_unique<cppflow::model>(CONF_FILTER_MODEL);
        }
        /* step 2: Load the image & convert to tensor */
        const ObjectWithStringKey *tcss_value = reinterpret_cast<const ObjectWithStringKey *>(value_ptr);
        const FrameData *frame = reinterpret_cast<const FrameData*>(tcss_value->blob.bytes);
        dbg_default_trace("frame photoid is: "+std::to_string(frame->photo_id));
        dbg_default_trace("frame timestamp is: "+std::to_string(frame->timestamp));
    
        std::vector<float> tensor_buf(FILTER_TENSOR_BUFFER_SIZE);
        std::memcpy(static_cast<void*>(tensor_buf.data()),static_cast<const void*>(frame->data), sizeof(frame->data));
        cppflow::tensor input_tensor(std::move(tensor_buf), {IMAGE_WIDTH,IMAGE_HEIGHT,3});
        input_tensor = cppflow::expand_dims(input_tensor, 0);
        
        /* step 3: Predict */
        cppflow::tensor output = (*model)({{"serving_default_conv2d_3_input:0", input_tensor}},{"StatefulPartitionedCall:0"})[0];
        
        /* step 4: Send intermediate results to the next tier if image frame is meaningful */
        // prediction < 0.35 indicates strong possibility that the image frame captures full contour of the cow
        float prediction = output.get_data<float>()[0];
        // std::cout << "prediction: " << prediction << std::endl;
#ifdef ENABLE_EVALUATION
        if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
            global_timestamp_logger.log(TLT_FRONTEND_PREDICTED,
                                        typed_ctxt->get_service_client_ref().get_my_id(),
                                        tcss_value->get_message_id(),
                                        get_walltime());
        }
#endif
        if (prediction < FILTER_THRESHOLD) {
            std::string frame_idx = key_string.substr(prefix_length);
            for (auto iter = outputs.begin(); iter != outputs.end(); ++iter) {
                std::string obj_key = iter->first + frame_idx;
                ObjectWithStringKey obj(obj_key,tcss_value->blob.bytes,tcss_value->blob.size);
#ifdef ENABLE_EVALUATION
                if (std::is_base_of<IHasMessageID,std::decay_t<ObjectWithStringKey>>::value) {
                    obj.set_message_id(tcss_value->get_message_id());
                }
#endif
                std::lock_guard<std::mutex> lock(p2p_send_mutex);
                
                // if true, use trigger put; otherwise, use normal put
                if (iter->second) {
                    if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
                        dbg_default_trace("trigger put output obj (key:{}, id:{}).", obj.get_key_ref(), obj.get_message_id());
                    }
                    auto result = typed_ctxt->get_service_client_ref().trigger_put(obj);
                    result.get();
                    if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
                        dbg_default_trace("finish trigger put obj (key:{}, id{}).", obj.get_key_ref(), obj.get_message_id());
                    }
                } 
                else {
                    if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
                        dbg_default_trace("put output obj (key:{}, id:{}).", obj.get_key_ref(), obj.get_message_id());
                    }
                    typed_ctxt->get_service_client_ref().put_and_forget(obj);
                    if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
                        dbg_default_trace("finish put obj (key:{}, id{}).", obj.get_key_ref(), obj.get_message_id());
                    }
                }
            }
        }
#ifdef ENABLE_EVALUATION
        if (std::is_base_of<IHasMessageID,ObjectWithStringKey>::value) {
            global_timestamp_logger.log(TLT_FRONTEND_FORWARDED,
                                        typed_ctxt->get_service_client_ref().get_my_id(),
                                        tcss_value->get_message_id(),
                                        get_walltime());
        }
#endif
    }

    static std::shared_ptr<OffCriticalDataPathObserver> ocdpo_ptr;
public:
    static void initialize() {
        if(!ocdpo_ptr) {
            ocdpo_ptr = std::make_shared<DairyFarmFilterOCDPO>();
        }
    }
    static auto get() {
        return ocdpo_ptr;
    }
};

std::mutex DairyFarmFilterOCDPO::init_mutex;
std::shared_ptr<OffCriticalDataPathObserver> DairyFarmFilterOCDPO::ocdpo_ptr;

void initialize(ICascadeContext* ctxt) {
#ifdef ENABLE_GPU
    auto* typed_ctxt = dynamic_cast<DefaultCascadeContextType*>(ctxt);
    /* Configure GPU context for tensorflow */
    if (typed_ctxt->resource_descriptor.gpus.size()==0) {
        dbg_default_error("GPU is requested but no GPU found...giving up on processing data.");
        return;
    }
    std::cout << "Configuring tensorflow GPU context" << std::endl;
    // Serialized config options (example of 30% memory fraction)
    // TODO: configure gpu settings, link: https://serizba.github.io/cppflow/quickstart.html#gpu-config-options
    // std::vector<uint8_t> config{0x32,0x9,0x9,0x9a,0x99,0x99,0x99,0x99,0x99,0xb9,0x3f};
    std::vector<uint8_t> config{DEFAULT_TFE_CONFIG};
    // Create new options with your configuration
    TFE_ContextOptions* options = TFE_NewContextOptions();
    TFE_ContextOptionsSetConfig(options, config.data(), config.size(), cppflow::context::get_status());
    // Replace the global context with your options
    cppflow::get_global_context() = cppflow::context(options);
#endif 
    DairyFarmFilterOCDPO::initialize();
}

std::shared_ptr<OffCriticalDataPathObserver> get_observer(
        ICascadeContext*,const nlohmann::json&) {
    return DairyFarmFilterOCDPO::get();
}

void release(ICascadeContext* ctxt) {
    // nothing to release
    return;
}

} // namespace cascade
} // namespace derecho
