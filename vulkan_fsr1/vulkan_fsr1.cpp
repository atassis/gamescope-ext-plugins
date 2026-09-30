// vulkan-fsr1: an external_upscaler.h plugin that runs gamescope's own FSR1 (EASU+RCAS) math on a
// Vulkan device chosen independently of the game's/gamescope's render device -- the ABI's proof
// that the upscale step can run on a second GPU, importing/exporting dma-bufs cross-device. Config
// string: "uuid:<deviceUUID as vulkaninfo prints it>" or "index:<physical device index>" (default:
// index 0, i.e. whatever vkEnumeratePhysicalDevices lists first -- pass GAMESCOPE_VULKAN_FSR1_LIST=1
// to print the list and exit create() with failure). uuid survives a device being added or removed.
#include "external_upscaler.h"

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <poll.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <vector>

#include "easu.h"
#include "rcas.h"

// AMD FSR1 host-side constant setup (ffx_a.h/ffx_fsr1.h, same math the shaders use); included with
// the GLSL guards off so it compiles as plain C++.
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace
{
	struct Vk
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physDev = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t queueFamily = 0;
		VkCommandPool cmdPool = VK_NULL_HANDLE;
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline easuPipeline = VK_NULL_HANDLE;
		VkPipeline rcasPipeline = VK_NULL_HANDLE;
		VkDescriptorPool descPool = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		gs_upscaler_device_info_t deviceInfo = {};
		bool bHaveExternalSemaphore = false;
		VkSemaphore outSem = VK_NULL_HANDLE;   // exported as a sync_file fd each submit(), reusable (spec: exporting SYNC_FD resets it to unsignaled)
		VkFence workFence = VK_NULL_HANDLE;    // signals when the frame's GPU work (and thus outSem) is done, so the NEXT submit() knows it's safe to free THIS frame's resources
		PFN_vkGetSemaphoreFdKHR pfnGetSemaphoreFdKHR = nullptr;
	};

	// One call's transient resources (imported in/out images, the temp EASU target, descriptor
	// sets): freed at the START of the next submit(), gated on workFence, not at the end of this
	// one -- freeing them before the GPU work they're used by finishes would be a use-after-free
	// now that submit() returns before that work completes (see fsr1_submit).
	struct PendingFree
	{
		bool bValid = false;
		VkImage inImage = VK_NULL_HANDLE, tmpImage = VK_NULL_HANDLE, outImage = VK_NULL_HANDLE;
		VkDeviceMemory inMem = VK_NULL_HANDLE, tmpMem = VK_NULL_HANDLE, outMem = VK_NULL_HANDLE;
		VkImageView inView = VK_NULL_HANDLE, tmpView = VK_NULL_HANDLE, outView = VK_NULL_HANDLE;
		VkDescriptorSet easuSet = VK_NULL_HANDLE, rcasSet = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
	};

	void fail( const char *psz )
	{
		fprintf( stderr, "[vulkan-fsr1] %s\n", psz );
	}

	uint32_t div_roundup( uint32_t n, uint32_t d ) { return ( n + d - 1 ) / d; }

	VkShaderModule makeShader( VkDevice dev, const uint32_t *pCode, size_t size )
	{
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = size;
		ci.pCode = pCode;
		VkShaderModule mod = VK_NULL_HANDLE;
		vkCreateShaderModule( dev, &ci, nullptr, &mod );
		return mod;
	}
}

extern "C" struct gs_upscaler_instance
{
	Vk vk;
	uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
	PendingFree pending;
};

namespace
{
	bool hasExtension( const std::vector<VkExtensionProperties> &exts, const char *name )
	{
		for ( auto &e : exts )
			if ( strcmp( e.extensionName, name ) == 0 )
				return true;
		return false;
	}

	bool initVulkan( gs_upscaler_instance *inst, const char *config )
	{
		Vk &vk = inst->vk;

		VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		appInfo.pApplicationName = "gamescope-vulkan-fsr1";
		appInfo.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &appInfo;
		if ( vkCreateInstance( &ici, nullptr, &vk.instance ) != VK_SUCCESS )
		{
			fail( "vkCreateInstance failed" );
			return false;
		}

		uint32_t nPhys = 0;
		vkEnumeratePhysicalDevices( vk.instance, &nPhys, nullptr );
		std::vector<VkPhysicalDevice> physDevs( nPhys );
		vkEnumeratePhysicalDevices( vk.instance, &nPhys, physDevs.data() );
		if ( nPhys == 0 )
		{
			fail( "no Vulkan physical devices" );
			return false;
		}

		auto uuidString = []( VkPhysicalDevice dev )
		{
			VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
			vkGetPhysicalDeviceProperties2( dev, &props2 );
			char sz[37];
			char *p = sz;
			for ( int i = 0; i < VK_UUID_SIZE; i++ )
				p += sprintf( p, ( i == 4 || i == 6 || i == 8 || i == 10 ) ? "-%02x" : "%02x", id.deviceUUID[i] );
			return std::string( sz );
		};

		uint32_t nWantIndex = 0;
		if ( config && strncmp( config, "index:", 6 ) == 0 )
			nWantIndex = strtoul( config + 6, nullptr, 10 );
		else if ( config && strncmp( config, "uuid:", 5 ) == 0 )
		{
			nWantIndex = nPhys;
			for ( uint32_t i = 0; i < nPhys; i++ )
				if ( uuidString( physDevs[i] ) == config + 5 )
					nWantIndex = i;
		}
		if ( getenv( "GAMESCOPE_VULKAN_FSR1_LIST" ) )
		{
			for ( uint32_t i = 0; i < nPhys; i++ )
			{
				VkPhysicalDeviceProperties props;
				vkGetPhysicalDeviceProperties( physDevs[i], &props );
				fprintf( stderr, "[vulkan-fsr1] index %u: %s (type %d) uuid:%s\n", i, props.deviceName, props.deviceType, uuidString( physDevs[i] ).c_str() );
			}
			return false;
		}
		if ( nWantIndex >= nPhys )
		{
			fail( "no such device (index out of range or uuid not found)" );
			return false;
		}
		vk.physDev = physDevs[nWantIndex];

		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( vk.physDev, &props );
		snprintf( vk.deviceInfo.name, sizeof( vk.deviceInfo.name ), "vulkan:%s", props.deviceName );
		vk.deviceInfo.kind = GS_UPSCALER_DEVICE_GPU;
		vk.deviceInfo.size = sizeof( vk.deviceInfo );

		// Best-effort DRM render-node identity for the device-placement decision in
		// external_upscaler.h §1; not fatal if the extension is absent (dev_node stays 0, gamescope
		// then has to assume the plugin's device is not its own render device).
		uint32_t nDevExtCount = 0;
		vkEnumerateDeviceExtensionProperties( vk.physDev, nullptr, &nDevExtCount, nullptr );
		std::vector<VkExtensionProperties> devExts( nDevExtCount );
		vkEnumerateDeviceExtensionProperties( vk.physDev, nullptr, &nDevExtCount, devExts.data() );

		if ( hasExtension( devExts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME ) )
		{
			VkPhysicalDeviceDrmPropertiesEXT drmProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
			VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drmProps };
			vkGetPhysicalDeviceProperties2( vk.physDev, &props2 );
			if ( drmProps.hasRender )
				vk.deviceInfo.dev_node = makedev( drmProps.renderMajor, drmProps.renderMinor );
			else if ( drmProps.hasPrimary )
				vk.deviceInfo.dev_node = makedev( drmProps.primaryMajor, drmProps.primaryMinor );
		}

		uint32_t nQueueFamilies = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( vk.physDev, &nQueueFamilies, nullptr );
		std::vector<VkQueueFamilyProperties> qprops( nQueueFamilies );
		vkGetPhysicalDeviceQueueFamilyProperties( vk.physDev, &nQueueFamilies, qprops.data() );
		vk.queueFamily = UINT32_MAX;
		for ( uint32_t i = 0; i < nQueueFamilies; i++ )
		{
			if ( qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT )
			{
				vk.queueFamily = i;
				break;
			}
		}
		if ( vk.queueFamily == UINT32_MAX )
		{
			fail( "no compute queue family" );
			return false;
		}

		const char *pRequired[] = {
			VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
			VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
			VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
			VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, // pulled in by drm-format-modifier on some drivers
		};
		std::vector<const char *> enable;
		for ( auto *p : pRequired )
		{
			if ( !hasExtension( devExts, p ) )
			{
				fprintf( stderr, "[vulkan-fsr1] device is missing required extension %s\n", p );
				return false;
			}
			enable.push_back( p );
		}
		// Optional: a real async out-fence (VK_KHR_external_semaphore_fd, SYNC_FD handle type).
		// Without it, submit() falls back to vkQueueWaitIdle and returns -1 -- correct, just not
		// the async path GAMESCOPE_VULKAN_FSR1_SYNC=1 also forces for comparison.
		vk.bHaveExternalSemaphore = hasExtension( devExts, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME ) &&
		                            hasExtension( devExts, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME );
		if ( vk.bHaveExternalSemaphore && !( getenv( "GAMESCOPE_VULKAN_FSR1_SYNC" ) && atoi( getenv( "GAMESCOPE_VULKAN_FSR1_SYNC" ) ) ) )
		{
			enable.push_back( VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME );
			enable.push_back( VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME );
		}
		else
		{
			vk.bHaveExternalSemaphore = false;
		}

		float prio = 1.0f;
		VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		qci.queueFamilyIndex = vk.queueFamily;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;
		VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = (uint32_t)enable.size();
		dci.ppEnabledExtensionNames = enable.data();
		if ( vkCreateDevice( vk.physDev, &dci, nullptr, &vk.device ) != VK_SUCCESS )
		{
			fail( "vkCreateDevice failed" );
			return false;
		}
		vkGetDeviceQueue( vk.device, vk.queueFamily, 0, &vk.queue );

		VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		vkCreateFence( vk.device, &fci, nullptr, &vk.workFence );

		if ( vk.bHaveExternalSemaphore )
		{
			vk.pfnGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr( vk.device, "vkGetSemaphoreFdKHR" );
			VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
			exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			VkSemaphoreCreateInfo semCi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo };
			if ( !vk.pfnGetSemaphoreFdKHR || vkCreateSemaphore( vk.device, &semCi, nullptr, &vk.outSem ) != VK_SUCCESS )
			{
				fprintf( stderr, "[vulkan-fsr1] exportable semaphore setup failed, falling back to synchronous submit\n" );
				vk.bHaveExternalSemaphore = false;
			}
		}

		VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpci.queueFamilyIndex = vk.queueFamily;
		vkCreateCommandPool( vk.device, &cpci, nullptr, &vk.cmdPool );

		VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sci.magFilter = VK_FILTER_LINEAR;
		sci.minFilter = VK_FILTER_LINEAR;
		sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		vkCreateSampler( vk.device, &sci, nullptr, &vk.sampler );

		VkDescriptorSetLayoutBinding bindings[2] = {};
		bindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		dslci.bindingCount = 2;
		dslci.pBindings = bindings;
		vkCreateDescriptorSetLayout( vk.device, &dslci, nullptr, &vk.setLayout );

		VkPushConstantRange pcRange = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 }; // max(sizeof(easu consts)=64, sizeof(rcas con)=16)
		VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &vk.setLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = &pcRange;
		vkCreatePipelineLayout( vk.device, &plci, nullptr, &vk.pipelineLayout );

		VkShaderModule easuMod = makeShader( vk.device, easu, sizeof( easu ) );
		VkShaderModule rcasMod = makeShader( vk.device, rcas, sizeof( rcas ) );
		VkComputePipelineCreateInfo cpci2[2] = {};
		cpci2[0] = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		cpci2[0].stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, easuMod, "main", nullptr };
		cpci2[0].layout = vk.pipelineLayout;
		cpci2[1] = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		cpci2[1].stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, rcasMod, "main", nullptr };
		cpci2[1].layout = vk.pipelineLayout;
		VkPipeline pipelines[2] = {};
		if ( vkCreateComputePipelines( vk.device, VK_NULL_HANDLE, 2, cpci2, nullptr, pipelines ) != VK_SUCCESS )
		{
			fail( "vkCreateComputePipelines failed" );
			return false;
		}
		vk.easuPipeline = pipelines[0];
		vk.rcasPipeline = pipelines[1];
		vkDestroyShaderModule( vk.device, easuMod, nullptr );
		vkDestroyShaderModule( vk.device, rcasMod, nullptr );

		VkDescriptorPoolSize poolSizes[2] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 },
		};
		VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		dpci.maxSets = 8;
		dpci.poolSizeCount = 2;
		dpci.pPoolSizes = poolSizes;
		vkCreateDescriptorPool( vk.device, &dpci, nullptr, &vk.descPool );

		return true;
	}

	// Imports a linear, single-plane dma-buf as a VkImage. Takes ownership of a DUP of fdIn (the
	// caller's fd is never closed here, matching external_upscaler.h's "borrowed" contract); the
	// dup is consumed by vkAllocateMemory on success and freed by vkFreeMemory on cleanup.
	bool importDmabufImage( Vk &vk, int fdIn, uint32_t w, uint32_t h, uint32_t stride, VkImageUsageFlags usage,
	                         VkImage &outImage, VkDeviceMemory &outMemory, VkImageView &outView )
	{
		VkSubresourceLayout layout = {};
		layout.rowPitch = stride;
		VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
		modInfo.drmFormatModifier = 0; // DRM_FORMAT_MOD_LINEAR
		modInfo.drmFormatModifierPlaneCount = 1;
		modInfo.pPlaneLayouts = &layout;
		VkExternalMemoryImageCreateInfo extInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modInfo };
		extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

		VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extInfo };
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_B8G8R8A8_UNORM; // DRM_FORMAT_ARGB8888 byte order
		ici.extent = { w, h, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ici.usage = usage;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if ( vkCreateImage( vk.device, &ici, nullptr, &outImage ) != VK_SUCCESS )
			return false;

		int fdDup = dup( fdIn );
		if ( fdDup < 0 )
			return false;

		VkMemoryFdPropertiesKHR fdProps = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
		auto pfnGetFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr( vk.device, "vkGetMemoryFdPropertiesKHR" );
		pfnGetFdProps( vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fdDup, &fdProps );

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements( vk.device, outImage, &memReq );

		uint32_t typeIndex = UINT32_MAX;
		VkPhysicalDeviceMemoryProperties memProps;
		vkGetPhysicalDeviceMemoryProperties( vk.physDev, &memProps );
		uint32_t candidates = memReq.memoryTypeBits & fdProps.memoryTypeBits;
		for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
		{
			if ( candidates & ( 1u << i ) )
			{
				typeIndex = i;
				break;
			}
		}
		if ( typeIndex == UINT32_MAX )
		{
			close( fdDup );
			return false;
		}

		VkImportMemoryFdInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
		importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		importInfo.fd = fdDup;
		VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &importInfo };
		dedicated.image = outImage;
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated };
		mai.allocationSize = memReq.size;
		mai.memoryTypeIndex = typeIndex;
		if ( vkAllocateMemory( vk.device, &mai, nullptr, &outMemory ) != VK_SUCCESS )
		{
			close( fdDup ); // import failed, fd was not consumed
			return false;
		}
		// fdDup is now owned by outMemory; do not close it.
		vkBindImageMemory( vk.device, outImage, outMemory, 0 );

		VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		ivci.image = outImage;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
		ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		return vkCreateImageView( vk.device, &ivci, nullptr, &outView ) == VK_SUCCESS;
	}
}

extern "C" gs_upscaler_instance *fsr1_create( const gs_upscaler_create_desc_t *desc )
{
	auto *inst = new gs_upscaler_instance();
	if ( !initVulkan( inst, desc && desc->config ? desc->config : nullptr ) )
	{
		delete inst;
		return nullptr;
	}
	return inst;
}

// Frees a completed call's transient resources; caller must already know workFence is signaled
// (or be willing to block, which is exactly what this does -- see fsr1_submit's header comment).
static void reapPending( gs_upscaler_instance *inst )
{
	Vk &vk = inst->vk;
	PendingFree &p = inst->pending;
	if ( !p.bValid )
		return;
	vkWaitForFences( vk.device, 1, &vk.workFence, VK_TRUE, UINT64_MAX );
	if ( p.cmd ) vkFreeCommandBuffers( vk.device, vk.cmdPool, 1, &p.cmd );
	if ( p.easuSet ) vkFreeDescriptorSets( vk.device, vk.descPool, 1, &p.easuSet );
	if ( p.rcasSet ) vkFreeDescriptorSets( vk.device, vk.descPool, 1, &p.rcasSet );
	if ( p.tmpView ) vkDestroyImageView( vk.device, p.tmpView, nullptr );
	if ( p.tmpImage ) vkDestroyImage( vk.device, p.tmpImage, nullptr );
	if ( p.tmpMem ) vkFreeMemory( vk.device, p.tmpMem, nullptr );
	if ( p.inView ) vkDestroyImageView( vk.device, p.inView, nullptr );
	if ( p.inImage ) vkDestroyImage( vk.device, p.inImage, nullptr );
	if ( p.inMem ) vkFreeMemory( vk.device, p.inMem, nullptr );
	if ( p.outView ) vkDestroyImageView( vk.device, p.outView, nullptr );
	if ( p.outImage ) vkDestroyImage( vk.device, p.outImage, nullptr );
	if ( p.outMem ) vkFreeMemory( vk.device, p.outMem, nullptr );
	p = PendingFree{};
}

extern "C" void fsr1_destroy( gs_upscaler_instance *inst )
{
	if ( !inst )
		return;
	Vk &vk = inst->vk;
	if ( vk.device )
	{
		vkDeviceWaitIdle( vk.device );
		reapPending( inst );
		if ( vk.outSem ) vkDestroySemaphore( vk.device, vk.outSem, nullptr );
		if ( vk.workFence ) vkDestroyFence( vk.device, vk.workFence, nullptr );
		if ( vk.descPool ) vkDestroyDescriptorPool( vk.device, vk.descPool, nullptr );
		if ( vk.easuPipeline ) vkDestroyPipeline( vk.device, vk.easuPipeline, nullptr );
		if ( vk.rcasPipeline ) vkDestroyPipeline( vk.device, vk.rcasPipeline, nullptr );
		if ( vk.pipelineLayout ) vkDestroyPipelineLayout( vk.device, vk.pipelineLayout, nullptr );
		if ( vk.setLayout ) vkDestroyDescriptorSetLayout( vk.device, vk.setLayout, nullptr );
		if ( vk.sampler ) vkDestroySampler( vk.device, vk.sampler, nullptr );
		if ( vk.cmdPool ) vkDestroyCommandPool( vk.device, vk.cmdPool, nullptr );
		vkDestroyDevice( vk.device, nullptr );
	}
	if ( vk.instance )
		vkDestroyInstance( vk.instance, nullptr );
	delete inst;
}

extern "C" void fsr1_get_device_info( gs_upscaler_instance *inst, gs_upscaler_device_info_t *out )
{
	*out = inst->vk.deviceInfo;
}

extern "C" void fsr1_negotiate( gs_upscaler_instance *inst, const gs_upscaler_negotiate_desc_t *desc, gs_upscaler_negotiate_result_t *out )
{
	memset( out, 0, sizeof( *out ) );
	out->size = sizeof( *out );
	if ( desc->hdr )
	{
		// v1: FSR1's EASU/RCAS run in gamma space (same assumption gamescope's own -F fsr uses);
		// declining an HDR layer is correct, not a limitation this plugin should silently paper over.
		out->accepted = false;
		return;
	}
	if ( getenv( "GAMESCOPE_VULKAN_FSR1_DECLINE" ) && atoi( getenv( "GAMESCOPE_VULKAN_FSR1_DECLINE" ) ) )
	{
		// Tests the host's decline -> GPU-FSR fallback against a plugin that refuses at negotiate().
		fprintf( stderr, "[vulkan-fsr1] GAMESCOPE_VULKAN_FSR1_DECLINE=1, declining\n" );
		out->accepted = false;
		return;
	}
	out->accepted = true;
	out->scale_num = 2;
	out->scale_den = 1;
	out->writes_into_host_buffer = true;
	out->format_count = 1;
	out->formats[0].size = sizeof( out->formats[0] );
	out->formats[0].plane = GS_UPSCALER_PLANE_BGRA8;
	out->formats[0].drm_fourcc = 0x34325241; // DRM_FORMAT_ARGB8888 ('AR24' little-endian fourcc)
	out->formats[0].drm_modifier = 0; // DRM_FORMAT_MOD_LINEAR
	out->formats[0].align_w = 1;
	out->formats[0].align_h = 1;

	inst->inW = desc->in_w; inst->inH = desc->in_h;
	inst->outW = desc->out_w; inst->outH = desc->out_h;
}

// v2: when bHaveExternalSemaphore, this is REAL async -- vkQueueSubmit signals vk.outSem and
// vk.workFence, submit() returns the semaphore exported as a sync_file fd without waiting for the
// GPU, and this call's transient resources are freed at the START of the NEXT submit() (reapPending,
// gated on workFence) rather than here. Without it (old driver, or GAMESCOPE_VULKAN_FSR1_SYNC=1),
// falls back to the v1 shape: vkQueueWaitIdle, free immediately, return -1.
extern "C" int fsr1_submit( gs_upscaler_instance *inst, const gs_upscaler_submit_t *submit )
{
	Vk &vk = inst->vk;
	reapPending( inst );

	if ( submit->in_fence_fd >= 0 )
	{
		struct pollfd pfd = { submit->in_fence_fd, POLLIN, 0 };
		poll( &pfd, 1, 1000 );
	}

	VkImage inImage = VK_NULL_HANDLE, tmpImage = VK_NULL_HANDLE, outImage = VK_NULL_HANDLE;
	VkDeviceMemory inMem = VK_NULL_HANDLE, tmpMem = VK_NULL_HANDLE, outMem = VK_NULL_HANDLE;
	VkImageView inView = VK_NULL_HANDLE, tmpView = VK_NULL_HANDLE, outView = VK_NULL_HANDLE;
	bool bOk = importDmabufImage( vk, submit->in.fd, submit->in.width, submit->in.height, submit->in.stride,
	                               VK_IMAGE_USAGE_SAMPLED_BIT, inImage, inMem, inView );
	bOk = bOk && importDmabufImage( vk, submit->out.fd, submit->out.width, submit->out.height, submit->out.stride,
	                                 VK_IMAGE_USAGE_STORAGE_BIT, outImage, outMem, outView );

	// EASU's target is a plain device-local storage image -- not exported, gone at the end of this call.
	if ( bOk )
	{
		VkImageCreateInfo tmpCi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		tmpCi.imageType = VK_IMAGE_TYPE_2D;
		tmpCi.format = VK_FORMAT_B8G8R8A8_UNORM;
		tmpCi.extent = { submit->out.width, submit->out.height, 1 };
		tmpCi.mipLevels = 1;
		tmpCi.arrayLayers = 1;
		tmpCi.samples = VK_SAMPLE_COUNT_1_BIT;
		tmpCi.tiling = VK_IMAGE_TILING_OPTIMAL;
		tmpCi.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		tmpCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		tmpCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bOk = vkCreateImage( vk.device, &tmpCi, nullptr, &tmpImage ) == VK_SUCCESS;
	}
	if ( bOk )
	{
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements( vk.device, tmpImage, &req );
		VkPhysicalDeviceMemoryProperties memProps;
		vkGetPhysicalDeviceMemoryProperties( vk.physDev, &memProps );
		uint32_t typeIndex = UINT32_MAX;
		for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
			if ( ( req.memoryTypeBits & ( 1u << i ) ) && ( memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) )
			{
				typeIndex = i;
				break;
			}
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = typeIndex;
		bOk = typeIndex != UINT32_MAX && vkAllocateMemory( vk.device, &mai, nullptr, &tmpMem ) == VK_SUCCESS;
		if ( bOk )
			vkBindImageMemory( vk.device, tmpImage, tmpMem, 0 );
		VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		ivci.image = tmpImage;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
		ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		bOk = bOk && vkCreateImageView( vk.device, &ivci, nullptr, &tmpView ) == VK_SUCCESS;
	}

	VkDescriptorSet easuSet = VK_NULL_HANDLE, rcasSet = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if ( bOk )
	{
		VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		dsai.descriptorPool = vk.descPool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts = &vk.setLayout;
		vkAllocateDescriptorSets( vk.device, &dsai, &easuSet );
		vkAllocateDescriptorSets( vk.device, &dsai, &rcasSet );

		VkDescriptorImageInfo inInfo = { vk.sampler, inView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo tmpStorageInfo = { VK_NULL_HANDLE, tmpView, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo tmpSampledInfo = { vk.sampler, tmpView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo outStorageInfo = { VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL };

		VkWriteDescriptorSet writes[4] = {};
		writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, easuSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &inInfo };
		writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, easuSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &tmpStorageInfo };
		writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, rcasSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tmpSampledInfo };
		writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, rcasSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outStorageInfo };
		vkUpdateDescriptorSets( vk.device, 4, writes, 0, nullptr );

		VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cbai.commandPool = vk.cmdPool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;
		vkAllocateCommandBuffers( vk.device, &cbai, &cmd );

		VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer( cmd, &cbbi );

		auto barrier = [&]( VkImage img, VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS )
		{
			VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.oldLayout = oldL; b.newLayout = newL;
			b.srcAccessMask = srcA; b.dstAccessMask = dstA;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier( cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b );
		};

		barrier( inImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
		barrier( tmpImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
		barrier( outImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.easuPipeline );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipelineLayout, 0, 1, &easuSet, 0, nullptr );
		struct { AU1 c1[4], c2[4], c3[4], c4[4]; } easuConsts;
		FsrEasuCon( easuConsts.c1, easuConsts.c2, easuConsts.c3, easuConsts.c4,
			(AF1)submit->in.width, (AF1)submit->in.height, (AF1)submit->in.width, (AF1)submit->in.height,
			(AF1)submit->out.width, (AF1)submit->out.height );
		vkCmdPushConstants( cmd, vk.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( easuConsts ), &easuConsts );
		vkCmdDispatch( cmd, div_roundup( submit->out.width, 16 ), div_roundup( submit->out.height, 16 ), 1 );

		barrier( tmpImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.rcasPipeline );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipelineLayout, 0, 1, &rcasSet, 0, nullptr );
		AU1 rcasCon[4];
		FsrRcasCon( rcasCon, 0.0f );
		vkCmdPushConstants( cmd, vk.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( rcasCon ), &rcasCon );
		vkCmdDispatch( cmd, div_roundup( submit->out.width, 8 ), div_roundup( submit->out.height, 8 ), 1 );

		barrier( outImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );

		vkEndCommandBuffer( cmd );

		vkResetFences( vk.device, 1, &vk.workFence );
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		if ( vk.bHaveExternalSemaphore )
		{
			si.signalSemaphoreCount = 1;
			si.pSignalSemaphores = &vk.outSem;
		}
		vkQueueSubmit( vk.queue, 1, &si, vk.workFence );

		if ( !vk.bHaveExternalSemaphore )
			vkQueueWaitIdle( vk.queue ); // sync fallback, see the file header comment
	}

	if ( !bOk )
	{
		if ( tmpView ) vkDestroyImageView( vk.device, tmpView, nullptr );
		if ( tmpImage ) vkDestroyImage( vk.device, tmpImage, nullptr );
		if ( tmpMem ) vkFreeMemory( vk.device, tmpMem, nullptr );
		if ( inView ) vkDestroyImageView( vk.device, inView, nullptr );
		if ( inImage ) vkDestroyImage( vk.device, inImage, nullptr );
		if ( inMem ) vkFreeMemory( vk.device, inMem, nullptr );
		if ( outView ) vkDestroyImageView( vk.device, outView, nullptr );
		if ( outImage ) vkDestroyImage( vk.device, outImage, nullptr );
		if ( outMem ) vkFreeMemory( vk.device, outMem, nullptr );
		if ( easuSet ) vkFreeDescriptorSets( vk.device, vk.descPool, 1, &easuSet );
		if ( rcasSet ) vkFreeDescriptorSets( vk.device, vk.descPool, 1, &rcasSet );
		if ( cmd ) vkFreeCommandBuffers( vk.device, vk.cmdPool, 1, &cmd );
		return -2;
	}

	if ( !vk.bHaveExternalSemaphore )
	{
		// Sync fallback already waited above (vkQueueWaitIdle): safe to free now, matches v1.
		vkFreeCommandBuffers( vk.device, vk.cmdPool, 1, &cmd );
		vkFreeDescriptorSets( vk.device, vk.descPool, 1, &easuSet );
		vkFreeDescriptorSets( vk.device, vk.descPool, 1, &rcasSet );
		vkDestroyImageView( vk.device, tmpView, nullptr );
		vkDestroyImage( vk.device, tmpImage, nullptr );
		vkFreeMemory( vk.device, tmpMem, nullptr );
		vkDestroyImageView( vk.device, inView, nullptr );
		vkDestroyImage( vk.device, inImage, nullptr );
		vkFreeMemory( vk.device, inMem, nullptr );
		vkDestroyImageView( vk.device, outView, nullptr );
		vkDestroyImage( vk.device, outImage, nullptr );
		vkFreeMemory( vk.device, outMem, nullptr );
		return -1;
	}

	// Real async: defer all of the above to the next submit()/destroy() (reapPending), gated on
	// vk.workFence, and export the semaphore this vkQueueSubmit signaled as a sync_file fd. Per
	// spec (VK_KHR_external_semaphore_fd), exporting with SYNC_FD resets outSem to unsignaled, so
	// it's reusable next call without recreating it.
	inst->pending = PendingFree{};
	inst->pending.bValid = true;
	inst->pending.inImage = inImage; inst->pending.inMem = inMem; inst->pending.inView = inView;
	inst->pending.tmpImage = tmpImage; inst->pending.tmpMem = tmpMem; inst->pending.tmpView = tmpView;
	inst->pending.outImage = outImage; inst->pending.outMem = outMem; inst->pending.outView = outView;
	inst->pending.easuSet = easuSet; inst->pending.rcasSet = rcasSet; inst->pending.cmd = cmd;

	VkSemaphoreGetFdInfoKHR getInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
	getInfo.semaphore = vk.outSem;
	getInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	int fd = -1;
	if ( vk.pfnGetSemaphoreFdKHR( vk.device, &getInfo, &fd ) != VK_SUCCESS || fd < 0 )
	{
		// Export failed: fall back to a blocking wait so the caller still gets a correct frame.
		vkWaitForFences( vk.device, 1, &vk.workFence, VK_TRUE, UINT64_MAX );
		return -1;
	}
	return fd;
}

static const gs_upscaler_api_t s_api = {
	sizeof( gs_upscaler_api_t ),
	GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION,
	fsr1_create,
	fsr1_destroy,
	fsr1_get_device_info,
	fsr1_negotiate,
	fsr1_submit,
};

extern "C" const gs_upscaler_api_t *gamescope_external_upscaler_get_api( uint32_t host_abi_version )
{
	if ( host_abi_version != GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION )
		return nullptr;
	return &s_api;
}
