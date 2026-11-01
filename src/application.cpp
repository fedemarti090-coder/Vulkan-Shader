#define VOLK_IMPLEMENTATION
#include <volk.h>
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include "application.h"
#include "utils.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>

/*

Creare tre veritici e mandarli alla graphic pipeline

Scrivere pixel per pixel il colore del triangolo

Mostrare a schermo il triangolo

*/

void Application::showError(const std::string &errorMessage) const{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errorMessage.c_str(), window);
}

bool Application::initialize(){

    if(SDL_InitSubSystem(SDL_INIT_VIDEO)){

        if(!SDL_Vulkan_LoadLibrary(nullptr)){
            showError(std::string("Errore caricamento Vulkan da SDL: ") + SDL_GetError());
            return false;
        }
        vulkanLibraryLoaded = true;

        window = SDL_CreateWindow("Vulkan Intro", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        
        if(!window){
            showError(std::string("Errore creazione finestra: ") + SDL_GetError());
            return false;
        }

        if(!initializeVulkan()){
            return false;
        }
    }else{
        showError(std::string("Errore inizializzazione SDL3: ") + SDL_GetError());
        return false;
    }

    return true;
}


void Application::run(){

    bool running = true;

    while(running){

        SDL_Event event{ 0 };

        while(SDL_PollEvent(&event)){

            if(event.type == SDL_EVENT_QUIT){
                running = false;
                break;
            }
            else if (event.type == SDL_EVENT_WINDOW_RESIZED){
                width = event.window.data1;
                height = event.window.data2;
                break;
            }
        }
        render();
    }
}

void Application::shutdown(){

    vkDeviceWaitIdle(device); // flush GPU per far completare tutti i processi

    if (timelineSemaphore){
		vkDestroySemaphore(device, timelineSemaphore, nullptr);
	}

	for (auto &res : frameResources){
		vkDestroySemaphore(device, res.imageAcquiredSemaphore, nullptr);
		vkDestroyCommandPool(device, res.commandPool, nullptr);
	}

	if (pipelineLayout){
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
	}

	if (pipeline){
		vkDestroyPipeline(device, pipeline, nullptr);
	}

    if (vertShader){
		vkDestroyShaderModule(device, vertShader, nullptr);
	}
    
    if (fragShader){
		vkDestroyShaderModule(device, fragShader, nullptr);
	}

    destroySwapchain();

    if(vmaAllocator){
        vmaDestroyAllocator(vmaAllocator);
    }

    if(surface){
        vkDestroySurfaceKHR(vulkanInstance, surface, nullptr);
    }

    if(device){
        vkDestroyDevice(device, nullptr);
    }

    if(vulkanInstance){
        vkDestroyInstance(vulkanInstance, nullptr);
    }

    volkFinalize();

    if(window){
        SDL_DestroyWindow(window);
    }

    if(vulkanLibraryLoaded){
        SDL_Vulkan_UnloadLibrary();
        vulkanLibraryLoaded = false;
    }

    SDL_Quit();
}

void Application::destroySwapchain(){

    for(VkImageView swapchainImgView : swapchainImageViews){
        vkDestroyImageView(device, swapchainImgView, nullptr);
    }

    swapchainImageViews.clear();

    for(VkSemaphore &semaphore : renderCompleteSemaphores){
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    renderCompleteSemaphores.clear();
    
    if(swapchain){
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = nullptr;
    }

    if(depthImageView){
        vkDestroyImageView(device, depthImageView, nullptr);
        vmaDestroyImage(vmaAllocator, depthImage, depthImageAllocation);
        depthImageView = nullptr;
    }
}

bool Application::initializeVulkan(){

    if(!createVulkanInstance()){
        showError("Errore istanza vulkan");
        return false;
    }

    if(!createSurface()){
        showError("Errore surface vulkan");
        return false;
    }

    if(physicalDevice = findPhysicalDevice(); !physicalDevice){ // !ricerca device (GPU) dispositivo
        showError("Errore no device fisici (vulkan)");
        return false;
    }

    if(!findGraphicsQueue()){
        showError("Errore no queue grafiche (vulkan)");
        return false;
    }

    if(!createDevice(physicalDevice)){
        showError("Errore creazione logica GPU device (vulkan)");
        return false;
    }

    if(!initializeVMA()){
        showError("Errore inizializzazione Vulkan Memory Allocator (vulkan)");
        return false;
    }

    if(!createSwapchain(width, height)){ // swap tra GPU (vram) e monitor per il frame, opposizione di buffer visibilie e buffer in calcolo
        showError("Errore creazione swapchain (vulkan)");
        return false;
    }

    if(!createShader()){ // per compiare shader (frag e vert) in SPIR-V (GLSL compilato)
        showError("Errore moduli shader (vulkan)");
        return false;
    }

    if(pipeline = createGraphicsPipeline(); !pipeline){
        showError("Errore inizializzazione pipeline grafica (vulkan)");
        return false;
    }

    if(!createSyncResource()){  // Serve sincronizzare GPU e CPU per non corrompere la memoria durante il rendering
        showError("Errore creazione risorse sync (vulkan)");
        return false;
    }

    /*
    La CPU invece di preparare e mandare un frame all GPU e aspettare la risposta appena dopo che il frame viene mandato in parallelo comincia a pensare gia al prossimo frame

    Con l attesa di risposta:

         [GPU]     [GPU]     [GPU]     [GPU]     [GPU]     [GPU]
    [CPU]     [CPU]     [CPU]     [CPU]     [CPU]     [CPU]      ...

    Con il calcolo in sync:

         [GPU][GPU][GPU][GPU][GPU][GPU][GPU][GPU][GPU][GPU][GPU]
    [CPU][CPU][CPU][CPU][CPU][CPU][CPU][CPU][CPU][CPU][CPU][CPU] ...

    ! la memoria che la GPU usa non deve essere toccata dalla CPU
    */

    if(!createCommandBuffer()){
        showError("Errore creazione command buffer (vulkan)");
        return false;
    }

    return true;

}

bool Application::createVulkanInstance(){

    if(volkInitialize() != VK_SUCCESS){
        showError("Errore inizializzazione Volk");
        return false;
    }

    VkApplicationInfo appInfo {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Shader Triangolo",
        .apiVersion = VulkanVersion,
    };

    uint32_t instExtCount = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&instExtCount);
    std::vector<const char *> requestedExtensions{
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
    };

    for (int i=0; i< instExtCount; i++){
        requestedExtensions.push_back(extensions[i]);
    }

    std::vector<const char *> requestedLayers{
        "VK_LAYER_KHRONOS_validation"
    };

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
    };

    VkInstanceCreateInfo instCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &debugInfo,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t>(requestedLayers.size()),
        .ppEnabledLayerNames = requestedLayers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(requestedExtensions.size()),
        .ppEnabledExtensionNames = requestedExtensions.data()
    };

    if(vkCreateInstance(&instCreateInfo, nullptr, &vulkanInstance) != VK_SUCCESS){
        return false;
    }

    volkLoadInstance(vulkanInstance);
    return true;

}

bool Application::createSurface(){

    if(!SDL_Vulkan_CreateSurface(window, vulkanInstance, nullptr, &surface)){
        return false;
    }

    return true;
}

VkPhysicalDevice Application::findPhysicalDevice(){

    uint32_t physDeviceCount = 0;
    vkEnumeratePhysicalDevices(vulkanInstance, &physDeviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physDeviceCount);
    vkEnumeratePhysicalDevices(vulkanInstance, &physDeviceCount, physicalDevices.data());

    VkPhysicalDevice physicalDevice = nullptr;
    if(physDeviceCount){
        physicalDevice = physicalDevices[0]; // Scegliamo di default la prima GPU

        for (auto &pDev : physicalDevices){
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pDev, &props);

            if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
                physicalDevice = pDev;
                break;
            }
        }
    }
    return physicalDevice;
}

bool Application::findGraphicsQueue(){
    uint32_t queueFamCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2 (physicalDevice, &queueFamCount, nullptr);
    std::vector<VkQueueFamilyProperties2> queueFamProps(queueFamCount,
    { .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamCount, queueFamProps.data());

    for(int currentFamIdx = 0; currentFamIdx < queueFamProps.size(); currentFamIdx ++){
        VkBool32 hasPresentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, currentFamIdx, surface, &hasPresentSupport);
    
        const auto &props = queueFamProps[currentFamIdx];

        if(props.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT && hasPresentSupport){
            gfxQueueFamIdx = currentFamIdx;
            return true;
        }
    
    };

    return false;

}

bool Application::createDevice(VkPhysicalDevice physicalDevice){

    VkPhysicalDeviceVulkan14Features supportedFeatures14{ 
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr };
	VkPhysicalDeviceVulkan13Features supportedFeatures13{ 
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14 };
	VkPhysicalDeviceVulkan12Features supportedFeatures12{ 
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13 };
	VkPhysicalDeviceFeatures2 supportedFeatures{ 
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures12 };

    vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);

    if (!supportedFeatures13.dynamicRendering || !supportedFeatures13.synchronization2 || !supportedFeatures12.timelineSemaphore){
		
        showError("Errore il Physical device non ha le feature richieste");
		return false;
	}

    VkPhysicalDeviceVulkan14Features features14	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
		.pNext = nullptr,
	};
	VkPhysicalDeviceVulkan13Features features13	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &features14,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE,
	};
	VkPhysicalDeviceVulkan12Features features12	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.timelineSemaphore = VK_TRUE
	};
	VkPhysicalDeviceFeatures2 features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12 };

    std::vector<float> queuePriorities{ 1.0f };

    VkDeviceQueueCreateInfo gfxQueueInfo {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfxQueueFamIdx,
        .queueCount = 1,
        .pQueuePriorities = queuePriorities.data()
    };

	const std::vector<const char *> deviceExtensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo devCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &gfxQueueInfo,
		.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
		.ppEnabledExtensionNames = deviceExtensions.data(),
		.pEnabledFeatures = nullptr
	};

	if (vkCreateDevice(physicalDevice, &devCreateInfo, nullptr, &device) != VK_SUCCESS)
	{
		return false;
	}

	vkGetDeviceQueue(device, gfxQueueFamIdx, 0, &gfxQueue); //Puntiamo alla Queue 0
	if (!gfxQueue)
	{
		showError("Errore connessione Grapic Queue (device)");
		return false;
	}
	return true;
}

bool Application::initializeVMA(){

    VmaVulkanFunctions vmaFuncInfo{};
    VmaAllocatorCreateInfo vmaAllocInfo {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, //per usare VRAM direttamente dagli shader
        .physicalDevice = physicalDevice,
        .device = device,
        .pVulkanFunctions = &vmaFuncInfo,
        .instance = vulkanInstance,
        .vulkanApiVersion = VulkanVersion
    };

    vmaImportVulkanFunctionsFromVolk(&vmaAllocInfo, &vmaFuncInfo);

    if(vmaCreateAllocator(&vmaAllocInfo, &vmaAllocator) != VK_SUCCESS){
        return false;
    }

    return true;
}

bool Application::createSwapchain(uint32_t width, uint32_t height){
    swapchainWidth = width;
    swapchainHeight = height;

    VkSurfaceCapabilitiesKHR surfaceCaps{};

    if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCaps) != VK_SUCCESS){
        showError("Errore surface incompatibile (swap chain)");
        return false;
    }

    uint32_t requestedImageCount = std::max(2u, surfaceCaps.minImageCount);
    if(surfaceCaps.maxImageCount > 0){
        requestedImageCount = std::min(requestedImageCount, surfaceCaps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainCreateInfo {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surface,
		.minImageCount = requestedImageCount,
		.imageFormat = swapchainFormat,
		.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR, //usiamo sRGB
		.imageExtent{.width = swapchainWidth, .height = swapchainHeight },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, //opacità finestra per l'OS
		.presentMode = VK_PRESENT_MODE_FIFO_KHR //per sincronizzare fotogrammi monitor e GPU (rimpiazza la fine della queue in questo caso)
	};

    if(vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain ) != VK_SUCCESS){
        
        showError("Errore crezione swapchain (swapchain)");
        return false;
    }

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
    swapchainImageViews.resize(imageCount);

    for(size_t i=0; i<swapchainImages.size(); ++i){
        VkImageViewCreateInfo imgViewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchainImages[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = swapchainFormat,
			.subresourceRange
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1
			}
		};

        if(vkCreateImageView(device, &imgViewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS){
            showError("Errore creazione image view (swapchain)");
            return false;
        }
    }

    renderCompleteSemaphores.resize(swapchainImages.size());

    for(VkSemaphore &semaphore : renderCompleteSemaphores){
        VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS){
            showError("Errore completazione render (swapchain/semaphore)");
            return false;
        }
    }

    // creiamo un depth buffer --> RGB + DEPTH(0.1)
    // cosi il render sarà creato parallelamente ma in base alla profondità

    VkImageCreateInfo depthCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthFormat,
        .extent{.width = swapchainWidth, .height = swapchainHeight, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo allocInfo {
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO
    };

    if(vmaCreateImage(vmaAllocator, &depthCreateInfo, &allocInfo, &depthImage, &depthImageAllocation, nullptr) != VK_SUCCESS){
        showError("Errore allocazione depth image (swapchain/depthBuffer)");
        return false;
    }

    VkImageViewCreateInfo depthImgViewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = depthImage,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = depthFormat,
		.subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1}
	};

    if(vkCreateImageView(device, &depthImgViewInfo, nullptr, &depthImageView) != VK_SUCCESS){
        showError("Errore creazione depth image (swapchain/depthimage)");
        return false;
    }

    return true;


}

VkShaderModule Application::createShaderModule(const std::string &filename, shaderc_shader_kind kind) const {
    
    const std::string shaderPath = "src/shaders/" + filename;
    const std::string src = readTextFile(shaderPath);

    if(src.empty()){
        showError("Errore file shader non esistente (shader/module)" + shaderPath);
        return nullptr;
    }

    std::cout<<"Compilazione shader: "<<shaderPath<< std::endl;

    shaderc::Compiler compiler; // compiler
    shaderc::CompileOptions opts; //opzioni compiler
    opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
	opts.SetTargetSpirv(shaderc_spirv_version_1_6);
	opts.SetOptimizationLevel(shaderc_optimization_level_performance);
	shaderc::CompilationResult result = compiler.CompileGlslToSpv(src, kind, filename.c_str(), opts);

    if(result.GetCompilationStatus() != shaderc_compilation_status_success){
        std::cerr<<"Errore compilazione shader (shader/compilazione)"<< result.GetErrorMessage() <<std::endl;
        return nullptr;
    }

    const size_t shaderSize = (result.cend() - result.cbegin()) * sizeof(uint32_t);

    VkShaderModuleCreateInfo moduleCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shaderSize,
		.pCode = result.cbegin()
	};

    VkShaderModule shaderModule = nullptr;
    if(vkCreateShaderModule(device, &moduleCreateInfo, nullptr, &shaderModule) != VK_SUCCESS){
        showError("Errore creazione shader module (shader/module)");
        return nullptr;
    }

    return shaderModule;

}

bool Application::createShader(){

    if(vertShader = createShaderModule("shader.vert", shaderc_vertex_shader); !vertShader){
        return false;
    }
    if(fragShader = createShaderModule("shader.frag", shaderc_fragment_shader); !fragShader){
        return false;
    }

    return true;
}

VkPipeline Application::createGraphicsPipeline(){

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pushConstantRangeCount = 0
    };

    if(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS){
        showError("Errore creazione layout per pipeline (pipeline/layout)");
        return nullptr;
    }

    const char *entryPoint = "main";
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShader,
            .pName = entryPoint
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShader,
            .pName = entryPoint
        }
    };

    VkPipelineVertexInputStateCreateInfo vertInputInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };

    // Assemblaggio degli shader
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };

    // configurazione depth
	VkPipelineDepthStencilStateCreateInfo depthStencilInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = VK_FALSE
	};

    VkPipelineViewportStateCreateInfo viewportInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr
    };


    // conversione da triangolo a lista di pixel
    VkPipelineRasterizationStateCreateInfo rasterInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, // Riempie l interno del triangolo
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multiSampleInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    // operazione di colorblending
    VkPipelineColorBlendAttachmentState attachState {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo blendInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachState
    };

    std::vector<VkDynamicState> dynamicState {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicStateInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = static_cast<uint32_t>(dynamicState.size()),
		.pDynamicStates = dynamicState.data()
    };

    VkPipelineRenderingCreateInfo renderInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &swapchainFormat,
		.depthAttachmentFormat = depthFormat
	};

	// creazione finale pipeline grafica
	VkGraphicsPipelineCreateInfo pipelineInfo
	{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &renderInfo,
		.stageCount = static_cast<uint32_t>(shaderStages.size()),
		.pStages = shaderStages.data(),
		.pVertexInputState = &vertInputInfo,
		.pInputAssemblyState = &inputAssemblyInfo,
		.pViewportState = &viewportInfo,
		.pRasterizationState = &rasterInfo,
		.pMultisampleState = &multiSampleInfo,
		.pDepthStencilState = &depthStencilInfo,
		.pColorBlendState = &blendInfo,
		.pDynamicState = &dynamicStateInfo,
		.layout = pipelineLayout,
		.renderPass = VK_NULL_HANDLE,
	};

	if (vkCreateGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
	{
		showError("Errore nella creazione pipeline (pipeline/final)");
		return nullptr;
	}


	return pipeline;
}

bool Application::createSyncResource(){

    VkSemaphoreTypeCreateInfo semaphoreTypeInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = MaxFramesInFlight
    };

    VkSemaphoreCreateInfo semaphoreInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &semaphoreTypeInfo
    };

    if(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timelineSemaphore) != VK_SUCCESS){
        showError("Errore creazione timeline semaphore (syncResource)");
        return false;
    }

    for (FrameResources &res : frameResources){
        VkSemaphoreCreateInfo semaphoreInfo { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        if(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &res.imageAcquiredSemaphore) != VK_SUCCESS){
            showError("Errore creazione pre-frame image-acquire semaphore (syncResource/semaphore)");
            return false;
        }
    }

    return true;
}

bool Application::createCommandBuffer(){

    for (FrameResources &res : frameResources){

        VkCommandPoolCreateInfo poolInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = gfxQueueFamIdx
        };

        if(vkCreateCommandPool(device, &poolInfo, nullptr, &res.commandPool) != VK_SUCCESS){
            showError("Errore creazione command buffer pool (commandBuffer)");
            return false;
        }

        VkCommandBufferAllocateInfo cmdAllocInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = res.commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
        };

        if(vkAllocateCommandBuffers(device, &cmdAllocInfo, &res.commandBuffer) != VK_SUCCESS){
            showError("Errore allocazione command buffer (commandBuffer/Allocate)");
            return false;        
        }
    }
}



void Application::render(){

    if(requireSwapchainRecreate){
        vkDeviceWaitIdle(device);
		destroySwapchain();
		createSwapchain(width, height);
		requireSwapchainRecreate = false;
    }

    const uint32_t frameResIndex = frameIndex++ % MaxFramesInFlight;
    const uint64_t signalValue = nextSignalValue++;
    const uint64_t waitValue = signalValue - MaxFramesInFlight;

    VkSemaphoreWaitInfo waitInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &timelineSemaphore,
		.pValues = &waitValue
    };

    vkWaitSemaphores(device, &waitInfo, UINT64_MAX);

    FrameResources &res = frameResources[frameResIndex];
    vkResetCommandPool(device, res.commandPool, 0);

    VkSemaphore imageAcquireSemaphore = frameResources[frameResIndex].imageAcquiredSemaphore;

    uint32_t imageIndex = 0;
	VkResult acquireResult = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAcquireSemaphore, VK_NULL_HANDLE, &imageIndex);


	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR){

		requireSwapchainRecreate = true;
		return;

	}else if (acquireResult == VK_SUBOPTIMAL_KHR){

    	requireSwapchainRecreate = true;
	}

	VkCommandBufferBeginInfo cmdBeginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	vkBeginCommandBuffer(res.commandBuffer, &cmdBeginInfo);

	std::vector<VkImageMemoryBarrier2> layoutBarriers{
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.image = swapchainImages[imageIndex],
			.subresourceRange
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			}
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, // both specified to control memory access at both stages (write)
			.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			.image = depthImage,
			.subresourceRange
			{
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			}
		}
	};

	VkDependencyInfo depInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = static_cast<uint32_t>(layoutBarriers.size()),
		.pImageMemoryBarriers = layoutBarriers.data()
	};

	vkCmdPipelineBarrier2(res.commandBuffer, &depInfo);

	VkRenderingAttachmentInfo colorAttachInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = swapchainImageViews[imageIndex],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, 
		.clearValue{.color{0.01f, 0.01f, 0.01f, 1}}
	};

	VkRenderingAttachmentInfo depthAttachInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = depthImageView,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, 
		.clearValue{.depthStencil{1.0f, 0}}
	};

	VkRenderingInfo renderingInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea
		{
			.offset{.x = 0, .y = 0},
			.extent{.width = swapchainWidth, .height = swapchainHeight}
		},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colorAttachInfo,
		.pDepthAttachment = &depthAttachInfo
	};

	vkCmdBeginRendering(res.commandBuffer, &renderingInfo);
	{
		VkViewport viewport{
			.x = 0, .y = 0,
			.width = static_cast<float>(swapchainWidth),
			.height = static_cast<float>(swapchainHeight)
		};

		vkCmdSetViewport(res.commandBuffer, 0, 1, &viewport);

		VkRect2D scissor{
			.offset{.x = 0, .y = 0 },
			.extent{.width = swapchainWidth, .height = swapchainHeight}
		};

		vkCmdSetScissor(res.commandBuffer, 0, 1, &scissor);

		vkCmdBindPipeline(res.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(res.commandBuffer, 3, 1, 0, 0);
	}

    vkCmdEndRendering(res.commandBuffer);

	VkImageMemoryBarrier2 presentLayoutBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE, // nothing is waiting, but the cache is flushed and layout is transition
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.image = swapchainImages[imageIndex],
		.subresourceRange
		{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		}
	};

	VkDependencyInfo presentDepInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &presentLayoutBarrier
	};

	vkCmdPipelineBarrier2(res.commandBuffer, &presentDepInfo);

	vkEndCommandBuffer(res.commandBuffer);

	VkSemaphoreSubmitInfo imageAcquireWaitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = imageAcquireSemaphore,
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT 
	};

    std::vector<VkSemaphoreSubmitInfo> semaphoreSignals{
		{ 
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = renderCompleteSemaphores[imageIndex],
			.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
		},
		{ 
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = timelineSemaphore,
			.value = signalValue,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
		}
	};

	VkCommandBufferSubmitInfo cmdSubmitInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = res.commandBuffer,
	};

	VkSubmitInfo2 submitInfo{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &imageAcquireWaitInfo, 
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmdSubmitInfo,
		.signalSemaphoreInfoCount = static_cast<uint32_t>(semaphoreSignals.size()),
		.pSignalSemaphoreInfos = semaphoreSignals.data()
	};

	vkQueueSubmit2(gfxQueue, 1, &submitInfo, VK_NULL_HANDLE);

	VkPresentInfoKHR presentInfo{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &renderCompleteSemaphores[imageIndex], 
		.swapchainCount = 1,
		.pSwapchains = &swapchain,
		.pImageIndices = &imageIndex,
		.pResults = nullptr
	};

	vkQueuePresentKHR(gfxQueue, &presentInfo);


}