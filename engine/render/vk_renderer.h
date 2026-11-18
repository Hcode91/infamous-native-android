// Vulkan presentation for Android: a textured-quad view for single images, and a world view made of
// chunks (separately added and removed pieces of geometry, culled against the view) sharing textures.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ANativeWindow;

namespace ina {

class VkRenderer {
public:
    ~VkRenderer();
    // Returns false and sets error() when the device cannot be used.
    bool init(ANativeWindow* window);
    void shutdown();
    bool setImage(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
    // Replaces the scene. `vertices` holds position xyz, normal xyz, uv per vertex (8 floats). Each batch
    // draws a range of `indices` with one texture; texture 0xFFFFFFFF means plain grey. While a scene is
    // set it is drawn instead of the image; an empty scene goes back to the image.
    struct Batch {
        uint32_t firstIndex = 0, indexCount = 0, texture = 0xFFFFFFFFu;
        uint32_t mode = 0;   // 0 opaque, 1 cutout (alpha-tested), 2 decal (blended); give batches in that order
    };
    struct SceneTexture {
        uint32_t width = 0, height = 0;
        std::vector<std::vector<uint8_t>> levels;   // RGBA8, largest first, each half the size of the last
    };
    bool setScene(const std::vector<float>& vertices, const std::vector<uint32_t>& indices, const std::vector<Batch>& batches,
                  const std::vector<SceneTexture>& textures);
    // World chunks. Each chunk owns its vertex and index buffers; its batches name textures by key (0 = grey).
    // Textures are shared between chunks and freed when the last chunk using them is removed. Must be called
    // on the thread that draws.
    struct ChunkBatch {
        uint32_t firstIndex = 0, indexCount = 0;
        uint64_t texture = 0;
        uint32_t mode = 0;   // as in Batch
    };
    bool hasTexture(uint64_t key) const;
    bool addTexture(uint64_t key, const SceneTexture& texture);   // a texture no chunk uses yet
    uint32_t addChunk(const std::vector<float>& vertices, const std::vector<uint32_t>& indices, const std::vector<ChunkBatch>& batches,
                      const float boxMin[3], const float boxMax[3]);   // returns the chunk id, 0 on failure
    // Replaces a chunk's vertex data in place (same vertex count as when it was added); for animated meshes.
    bool updateChunkVertices(uint32_t id, const std::vector<float>& vertices);
    void removeChunk(uint32_t id);
    void setChunkVisible(uint32_t id, bool visible);
    void clearChunks();
    size_t chunkCount() const { return chunks_.size(); }
    size_t textureCount() const { return textures_.size(); }
    std::vector<uint64_t> textureKeys() const;
    // Textures no chunk uses are kept for reuse until the total size passes `budgetBytes`; then the oldest
    // unused ones are freed.
    void trimTextures(size_t budgetBytes);
    // Chunks and triangles drawn in the last frame (after culling).
    size_t drawnChunks() const { return drawnChunks_; }
    size_t drawnTriangles() const { return drawnTriangles_; }
    // Orbit camera around `target`: yaw and pitch in radians, distance in world units. Z is up.
    void setCamera(const float target[3], float yaw, float pitch, float distance);
    // Draws one frame. Returns false on an error that needs a restart; a changed surface is handled inside.
    bool drawFrame();
    const std::string& error() const { return error_; }
    double lastFrameMs() const { return frameMs_; }

private:
    bool fail(const char* what, VkResult r);
    bool createSwapchain();
    void destroySwapchain();
    void destroyTexture();
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* src, VkBuffer* buf, VkDeviceMemory* mem);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags props) const;

    ANativeWindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout meshLayout_ = VK_NULL_HANDLE;
    VkPipeline meshPipeline_ = VK_NULL_HANDLE;
    VkPipeline decalPipeline_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkImage depth_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMem_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    bool uploadTexture(uint32_t w, uint32_t h, const std::vector<std::vector<uint8_t>>& levels, VkSampler sampler, GpuTexture* out);
    void destroyGpuTexture(GpuTexture& t);
    struct SharedTexture {
        GpuTexture gpu;
        uint32_t users = 0;
        size_t bytes = 0;
        uint64_t lastUsed = 0;   // counter value when its last user went away
    };
    size_t textureBytes_ = 0;
    uint64_t releaseCounter_ = 0;
    struct Chunk {
        VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE;
        VkDeviceMemory vmem = VK_NULL_HANDLE, imem = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        size_t vertexBytes = 0;
        std::vector<ChunkBatch> batches;
        float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
        bool visible = true;
    };
    void destroyChunk(Chunk& c);
    void releaseTexture(uint64_t key);
    void waitIdleFrame();
    std::map<uint64_t, SharedTexture> textures_;
    std::map<uint32_t, Chunk> chunks_;
    uint32_t nextChunk_ = 1;
    size_t drawnChunks_ = 0, drawnTriangles_ = 0;
    GpuTexture greyTexture_;
    VkSampler repeatSampler_ = VK_NULL_HANDLE;
    float camTarget_[3] = {0, 0, 0};
    float camYaw_ = 0.6f, camPitch_ = 0.5f, camDist_ = 100.0f;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkSurfaceFormatKHR format_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;

    VkImage texture_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMem_ = VK_NULL_HANDLE;
    VkImageView textureView_ = VK_NULL_HANDLE;
    uint32_t texW_ = 0, texH_ = 0;

    std::string error_;
    double frameMs_ = 0;
};

}  // namespace ina
