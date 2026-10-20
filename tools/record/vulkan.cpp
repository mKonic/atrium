// -k *_vulkan: FFmpeg's Vulkan video encoders, as gpu-screen-recorder's
// encoder/video/vulkan.c feeds them: the shader draws into two exportable
// Vulkan images (Y and UV) that GL sees through EXT_memory_object_fd, GL
// signals a shared semaphore when it's done, and each frame is copied into
// the encoder's own Vulkan frame on the GPU.

#include "encoder.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <unistd.h>

#include <cstdio>
#include <cstring>

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#define GL_RG16_EXT 0x822C
#endif

namespace atrium::record {

namespace {

class VulkanEncoder : public VideoEncoder {
public:
    ~VulkanEncoder() override {
        if (dev_) {
            if (fence_)
                vk_.DestroyFence(dev_, fence_, nullptr);
            if (pool_)
                vk_.DestroyCommandPool(dev_, pool_, nullptr);
            if (gl_done_)
                vk_.DestroySemaphore(dev_, gl_done_, nullptr);
            for (Shared& s : shared_) {
                if (s.image)
                    vk_.DestroyImage(dev_, s.image, nullptr);
                if (s.memory)
                    vk_.FreeMemory(dev_, s.memory, nullptr);
            }
        }
        if (gl_semaphore_ && gl_.DeleteSemaphores)
            gl_.DeleteSemaphores(1, &gl_semaphore_);
        for (Shared& s : shared_)
            if (s.gl_memory && gl_.DeleteMemoryObjects)
                gl_.DeleteMemoryObjects(1, &s.gl_memory);
    }

    const Planes* begin() override { return &planes_; }

    AVFrame* end() override {
        AVFrame* f = av_frame_alloc();
        if (av_hwframe_get_buffer(ctx_->hw_frames_ctx, f, 0) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        // GL hands the images over (in GENERAL layout) once it has drawn them.
        const GLuint textures[2] = {shared_[0].texture, shared_[1].texture};
        const GLenum layouts[2] = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
        gl_.SignalSemaphore(gl_semaphore_, 0, nullptr, 2, textures, layouts);
        glFlush();

        auto* vkf = reinterpret_cast<AVVkFrame*>(f->data[0]);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(ctx_->hw_frames_ctx->data);
        auto* fctx = static_cast<AVVulkanFramesContext*>(fc->hwctx);
        fctx->lock_frame(fc, vkf);
        // One multi-planar image, or one image a plane.
        const bool multiplane = vkf->img[1] == VK_NULL_HANDLE;

        vk_.ResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.BeginCommandBuffer(cmd_, &begin);
        VkImageMemoryBarrier barriers[4] = {};
        int nb = 0;
        for (Shared& s : shared_) {
            VkImageMemoryBarrier& b = barriers[nb++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.dstQueueFamilyIndex = family_;
            b.image = s.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        const int dst_images = multiplane ? 1 : 2;
        for (int i = 0; i < dst_images; i++) {
            VkImageMemoryBarrier& b = barriers[nb++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // everything in it is overwritten
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = vkf->img[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                               uint32_t(nb), barriers);
        for (int i = 0; i < 2; i++) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {multiplane ? (i == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT)
                                              : VK_IMAGE_ASPECT_COLOR_BIT,
                                   0, 0, 1};
            copy.extent = {uint32_t(planes_.width[i]), uint32_t(planes_.height[i]), 1};
            vk_.CmdCopyImage(cmd_, shared_[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkf->img[multiplane ? 0 : i],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        // Back to GL (GENERAL), released to it.
        for (int i = 0; i < 2; i++) {
            VkImageMemoryBarrier& b = barriers[i];
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = family_;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, barriers);
        vk_.EndCommandBuffer(cmd_);

        // Wait for GL and for whatever last used the frame (its timeline
        // semaphore); signal the frame's next value for the encoder.
        VkSemaphore waits[1 + AV_NUM_DATA_POINTERS];
        uint64_t wait_values[1 + AV_NUM_DATA_POINTERS];
        VkPipelineStageFlags stages[1 + AV_NUM_DATA_POINTERS];
        VkSemaphore signals[AV_NUM_DATA_POINTERS];
        uint64_t signal_values[AV_NUM_DATA_POINTERS];
        uint32_t nw = 0, ns = 0;
        waits[nw] = gl_done_;
        wait_values[nw] = 0;
        stages[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        for (int i = 0; i < dst_images; i++) {
            waits[nw] = vkf->sem[i];
            wait_values[nw] = vkf->sem_value[i];
            stages[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
            signals[ns] = vkf->sem[i];
            signal_values[ns++] = vkf->sem_value[i] + 1;
        }
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = nw;
        timeline.pWaitSemaphoreValues = wait_values;
        timeline.signalSemaphoreValueCount = ns;
        timeline.pSignalSemaphoreValues = signal_values;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = nw;
        submit.pWaitSemaphores = waits;
        submit.pWaitDstStageMask = stages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        submit.signalSemaphoreCount = ns;
        submit.pSignalSemaphores = signals;
        vk_.ResetFences(dev_, 1, &fence_);
        const bool ok = vk_.QueueSubmit(queue_, 1, &submit, fence_) == VK_SUCCESS;
        if (ok)
            vk_.WaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
        for (int i = 0; i < dst_images; i++) {
            vkf->sem_value[i]++;
            vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkf->access[i] = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            vkf->queue_family[i] = family_;
        }
        fctx->unlock_frame(fc, vkf);
        if (!ok)
            av_frame_free(&f);
        return f;
    }

protected:
    bool setup(Gpu& gpu, AVCodecContext* ctx, bool ten_bit, std::string& error) override {
        if (!load_gl()) {
            error = "GL has no EXT_memory_object_fd / EXT_semaphore_fd here";
            return false;
        }
        // The Vulkan device of the GPU the frames are on (derived from its DRM node).
        AVBufferRef* drm = nullptr;
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&drm, AV_HWDEVICE_TYPE_DRM, gpu.node().c_str(), nullptr, 0) < 0 ||
            av_hwdevice_ctx_create_derived(&device, AV_HWDEVICE_TYPE_VULKAN, drm, 0) < 0) {
            av_buffer_unref(&drm);
            error = "no Vulkan device on " + gpu.node();
            return false;
        }
        av_buffer_unref(&drm);
        auto* hw = static_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
        dev_ = hw->act_dev;
        if (!load_vk(hw)) {
            av_buffer_unref(&device);
            error = "Vulkan lacks external memory/semaphore fds";
            return false;
        }
        AVBufferRef* frames = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        fc->format = AV_PIX_FMT_VULKAN;
        fc->sw_format = ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        fc->width = ctx->width;
        fc->height = ctx->height;
        auto* fctx = static_cast<AVVulkanFramesContext*>(fc->hwctx);
        fctx->usage = VkImageUsageFlagBits(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
        if (av_hwframe_ctx_init(frames) < 0) {
            av_buffer_unref(&frames);
            error = "Vulkan frames";
            return false;
        }
        ctx->pix_fmt = AV_PIX_FMT_VULKAN;
        ctx->hw_frames_ctx = frames;

        // A queue that copies.
        family_ = UINT32_MAX;
        for (int i = 0; i < hw->nb_qf; i++)
            if (hw->qf[i].flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                family_ = uint32_t(hw->qf[i].idx);
                break;
            }
        if (family_ == UINT32_MAX) {
            error = "no Vulkan queue that copies";
            return false;
        }
        // FFmpeg may create its queues with flags (internally synchronized):
        // those are only reached through vkGetDeviceQueue2, with the same flags.
        VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        qi.flags = hw->queue_flags;
        qi.queueFamilyIndex = family_;
        qi.queueIndex = 0;
        vk_.GetDeviceQueue2(dev_, &qi, &queue_);
        if (!queue_) {
            error = "no Vulkan queue";
            return false;
        }
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk_.CreateCommandPool(dev_, &pool, nullptr, &pool_) != VK_SUCCESS ||
            vk_.CreateFence(dev_, &fence, nullptr, &fence_) != VK_SUCCESS) {
            error = "Vulkan command pool";
            return false;
        }
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vk_.AllocateCommandBuffers(dev_, &alloc, &cmd_);

        // The two shared images, and GL's view of them.
        const VkFormat vk_formats[2] = {ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
                                        ten_bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM};
        const GLenum gl_formats[2] = {ten_bit ? GLenum(GL_R16_EXT) : GLenum(GL_R8), ten_bit ? GLenum(GL_RG16_EXT) : GLenum(GL_RG8)};
        for (int i = 0; i < 2; i++) {
            planes_.width[i] = ctx->width / (i + 1);
            planes_.height[i] = ctx->height / (i + 1);
            if (!share(i, vk_formats[i], gl_formats[i], error))
                return false;
        }
        // GL → Vulkan: GL's drawing is done.
        VkExportSemaphoreCreateInfo export_sem{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        export_sem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sem.pNext = &export_sem;
        int fd = -1;
        VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        if (vk_.CreateSemaphore(dev_, &sem, nullptr, &gl_done_) != VK_SUCCESS ||
            (get.semaphore = gl_done_, get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
             vk_.GetSemaphoreFdKHR(dev_, &get, &fd) != VK_SUCCESS)) {
            error = "Vulkan semaphore for GL";
            return false;
        }
        gl_.GenSemaphores(1, &gl_semaphore_);
        gl_.ImportSemaphoreFd(gl_semaphore_, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);  // GL owns the fd now
        gpu.clear(planes_);
        return true;
    }

private:
    struct Shared {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        GLuint gl_memory = 0, texture = 0;
    };

    bool share(int i, VkFormat vk_format, GLenum gl_format, std::string& error) {
        Shared& s = shared_[i];
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = vk_format;
        info.extent = {uint32_t(planes_.width[i]), uint32_t(planes_.height[i]), 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.CreateImage(dev_, &info, nullptr, &s.image) != VK_SUCCESS) {
            error = "Vulkan image";
            return false;
        }
        VkMemoryRequirements req;
        vk_.GetImageMemoryRequirements(dev_, s.image, &req);
        VkPhysicalDeviceMemoryProperties props;
        vk_.GetPhysicalDeviceMemoryProperties(phys_, &props);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < props.memoryTypeCount; t++)
            if ((req.memoryTypeBits & (1u << t)) && (props.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = t;
                break;
            }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = s.image;
        VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_info.pNext = &dedicated;
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.pNext = &export_info;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (type == UINT32_MAX || vk_.AllocateMemory(dev_, &alloc, nullptr, &s.memory) != VK_SUCCESS ||
            vk_.BindImageMemory(dev_, s.image, s.memory, 0) != VK_SUCCESS) {
            error = "Vulkan image memory";
            return false;
        }
        VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        get.memory = s.memory;
        get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (vk_.GetMemoryFdKHR(dev_, &get, &fd) != VK_SUCCESS) {
            error = "Vulkan memory export";
            return false;
        }
        gl_.CreateMemoryObjects(1, &s.gl_memory);
        const GLint yes = GL_TRUE;
        gl_.MemoryObjectParameteriv(s.gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &yes);
        gl_.ImportMemoryFd(s.gl_memory, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);  // GL owns the fd now
        glGenTextures(1, &s.texture);
        glBindTexture(GL_TEXTURE_2D, s.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        gl_.TexStorageMem2D(GL_TEXTURE_2D, 1, gl_format, planes_.width[i], planes_.height[i], s.gl_memory, 0);
        glGenFramebuffers(1, &planes_.fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, planes_.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.texture, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok)
            error = "GL can't draw into the shared Vulkan image";
        return ok;
    }

    bool load_gl() {
        auto get = [](const char* n) { return eglGetProcAddress(n); };
        gl_.CreateMemoryObjects = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(get("glCreateMemoryObjectsEXT"));
        gl_.DeleteMemoryObjects = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(get("glDeleteMemoryObjectsEXT"));
        gl_.MemoryObjectParameteriv = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(get("glMemoryObjectParameterivEXT"));
        gl_.ImportMemoryFd = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(get("glImportMemoryFdEXT"));
        gl_.TexStorageMem2D = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(get("glTexStorageMem2DEXT"));
        gl_.GenSemaphores = reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(get("glGenSemaphoresEXT"));
        gl_.DeleteSemaphores = reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(get("glDeleteSemaphoresEXT"));
        gl_.ImportSemaphoreFd = reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(get("glImportSemaphoreFdEXT"));
        gl_.SignalSemaphore = reinterpret_cast<PFNGLSIGNALSEMAPHOREEXTPROC>(get("glSignalSemaphoreEXT"));
        const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        return exts && std::strstr(exts, "GL_EXT_memory_object_fd") && std::strstr(exts, "GL_EXT_semaphore_fd") &&
               gl_.CreateMemoryObjects && gl_.ImportMemoryFd && gl_.TexStorageMem2D && gl_.GenSemaphores &&
               gl_.ImportSemaphoreFd && gl_.SignalSemaphore;
    }

    bool load_vk(AVVulkanDeviceContext* hw) {
        phys_ = hw->phys_dev;
        auto inst_get = hw->get_proc_addr;
        auto dev_get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(inst_get(hw->inst, "vkGetDeviceProcAddr"));
        if (!dev_get)
            return false;
#define DEV(name) vk_.name = reinterpret_cast<PFN_vk##name>(dev_get(dev_, "vk" #name))
        DEV(GetDeviceQueue2);
        DEV(CreateCommandPool);
        DEV(DestroyCommandPool);
        DEV(AllocateCommandBuffers);
        DEV(ResetCommandBuffer);
        DEV(BeginCommandBuffer);
        DEV(EndCommandBuffer);
        DEV(CmdPipelineBarrier);
        DEV(CmdCopyImage);
        DEV(QueueSubmit);
        DEV(CreateFence);
        DEV(DestroyFence);
        DEV(ResetFences);
        DEV(WaitForFences);
        DEV(CreateImage);
        DEV(DestroyImage);
        DEV(GetImageMemoryRequirements);
        DEV(AllocateMemory);
        DEV(FreeMemory);
        DEV(BindImageMemory);
        DEV(CreateSemaphore);
        DEV(DestroySemaphore);
        DEV(GetMemoryFdKHR);
        DEV(GetSemaphoreFdKHR);
#undef DEV
        vk_.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            inst_get(hw->inst, "vkGetPhysicalDeviceMemoryProperties"));
        return vk_.GetMemoryFdKHR && vk_.GetSemaphoreFdKHR && vk_.GetPhysicalDeviceMemoryProperties;
    }

    struct {
        PFN_vkGetDeviceQueue2 GetDeviceQueue2;
        PFN_vkCreateCommandPool CreateCommandPool;
        PFN_vkDestroyCommandPool DestroyCommandPool;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
        PFN_vkResetCommandBuffer ResetCommandBuffer;
        PFN_vkBeginCommandBuffer BeginCommandBuffer;
        PFN_vkEndCommandBuffer EndCommandBuffer;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
        PFN_vkCmdCopyImage CmdCopyImage;
        PFN_vkQueueSubmit QueueSubmit;
        PFN_vkCreateFence CreateFence;
        PFN_vkDestroyFence DestroyFence;
        PFN_vkResetFences ResetFences;
        PFN_vkWaitForFences WaitForFences;
        PFN_vkCreateImage CreateImage;
        PFN_vkDestroyImage DestroyImage;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
        PFN_vkAllocateMemory AllocateMemory;
        PFN_vkFreeMemory FreeMemory;
        PFN_vkBindImageMemory BindImageMemory;
        PFN_vkCreateSemaphore CreateSemaphore;
        PFN_vkDestroySemaphore DestroySemaphore;
        PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
        PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    } vk_{};
    struct {
        PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjects;
        PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjects;
        PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameteriv;
        PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFd;
        PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2D;
        PFNGLGENSEMAPHORESEXTPROC GenSemaphores;
        PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphores;
        PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFd;
        PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphore;
    } gl_{};

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore gl_done_ = VK_NULL_HANDLE;
    GLuint gl_semaphore_ = 0;
    Shared shared_[2];
    Planes planes_;
};

} // namespace

std::unique_ptr<VideoEncoder> make_vulkan_encoder() {
    return std::make_unique<VulkanEncoder>();
}

} // namespace atrium::record
