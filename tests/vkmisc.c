/* Offscreen Vulkan test, step 6: queries and conditional rendering around
 * compute dispatches (fill.comp.spv next to the program).
 *  M1 two timestamps with a buffer fill between them.
 *  M2 a pipeline-statistics query counts compute shader invocations.
 *  M3 vkCmdCopyQueryPoolResults writes that count into a buffer.
 *  M4 conditional rendering: a dispatch under a zero condition writes
 *     nothing, under a non-zero one it writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 48
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(1);
}

static uint8_t *make_buffer(VkBuffer *buf, VkDeviceSize size)
{
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	uint8_t *map;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT },
		NULL, buf));
	vkGetBufferMemoryRequirements(dev, *buf, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) },
		NULL, &mem));
	CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
	memset(map, 0x55, size);
	return map;
}

static VkCommandBuffer begin(void)
{
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	return cmd;
}

static void run(VkCommandBuffer cmd)
{
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cmd }, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
}

#define LOCAL 64
#define GROUPS 16

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkBuffer b0, b1, bc;
	uint32_t *w0, *w1, *wc, pc[2] = { 3, 7 };
	uint64_t ts[2] = { 0, 0 }, stat = 0;
	VkCommandBuffer cmd;
	VkShaderModule mod;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	VkQueryPool qts, qst;
	static uint32_t code[16384];
	size_t code_size;
	FILE *f;
	uint32_t i, bad, t;
	void (*begin_cond)(VkCommandBuffer, const VkConditionalRenderingBeginInfoEXT *);
	void (*end_cond)(VkCommandBuffer);

	if (!(f = fopen("fill.comp.spv", "rb")))
		return 1;
	code_size = fread(code, 1, sizeof(code), f);
	fclose(f);

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	pdev = pd[0];
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s\n", props.deviceName);
	CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &(VkPhysicalDeviceConditionalRenderingFeaturesEXT){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
			.conditionalRendering = VK_TRUE },
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } },
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = (const char *[]){ "VK_EXT_conditional_rendering" },
		.pEnabledFeatures = &(VkPhysicalDeviceFeatures){
			.pipelineStatisticsQuery = VK_TRUE } }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));
	begin_cond = (void *)vkGetDeviceProcAddr(dev, "vkCmdBeginConditionalRenderingEXT");
	end_cond = (void *)vkGetDeviceProcAddr(dev, "vkCmdEndConditionalRenderingEXT");

	w0 = (uint32_t *)make_buffer(&b0, (GROUPS + 1) * LOCAL * 4);
	w1 = (uint32_t *)make_buffer(&b1, 4096);
	wc = (uint32_t *)make_buffer(&bc, 256);

	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_size, .pCode = code }, NULL, &mod));
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
		.pBindings = &(VkDescriptorSetLayoutBinding){
			.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } },
		NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &(VkPushConstantRange){
			VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 } }, NULL, &pl));
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
		.layout = pl }, NULL, &pipe));
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize){
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } }, 0, NULL);

	CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 }, NULL, &qts));
	CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = 1,
		.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT },
		NULL, &qst));

	/* M1 */
	cmd = begin();
	vkCmdResetQueryPool(cmd, qts, 0, 2);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qts, 0);
	vkCmdFillBuffer(cmd, b1, 0, 4096, 0x12345678);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, qts, 1);
	run(cmd);
	t = vkGetQueryPoolResults(dev, qts, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT);
	printf("M1 timestamps: result %d, %llu then %llu (%s), fill word %08x\n", t,
	       (unsigned long long)ts[0], (unsigned long long)ts[1],
	       ts[0] && ts[1] >= ts[0] ? "ordered" : "WRONG", w1[0]);

	/* M2, M3 */
	cmd = begin();
	vkCmdResetQueryPool(cmd, qst, 0, 1);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
	vkCmdBeginQuery(cmd, qst, 0, 0);
	vkCmdDispatch(cmd, GROUPS, 1, 1);
	vkCmdEndQuery(cmd, qst, 0);
	vkCmdCopyQueryPoolResults(cmd, qst, 0, 1, b1, 64, 8,
				  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	run(cmd);
	t = vkGetQueryPoolResults(dev, qst, 0, 1, sizeof(stat), &stat, 8, VK_QUERY_RESULT_64_BIT);
	printf("M2 compute invocations: result %d, %llu (expect %u)\n", t,
	       (unsigned long long)stat, GROUPS * LOCAL);
	printf("M3 copied query result: %u %u (expect %u 0), word before %08x\n",
	       w1[16], w1[17], GROUPS * LOCAL, w1[15]);

	/* M4 */
	for (t = 0; t < 2; t++) {
		memset(w0, 0x55, GROUPS * LOCAL * 4);
		wc[0] = t;
		cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
		vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
		begin_cond(cmd, &(VkConditionalRenderingBeginInfoEXT){
			.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
			.buffer = bc, .offset = 0 });
		vkCmdDispatch(cmd, GROUPS, 1, 1);
		end_cond(cmd);
		run(cmd);
		for (bad = 0, i = 0; i < GROUPS * LOCAL; i++)
			bad += w0[i] != 0x55555555;
		printf("M4 condition %u: %u of %u words written (expect %u)\n", t, bad,
		       GROUPS * LOCAL, t ? GROUPS * LOCAL : 0);
	}
	return 0;
}
