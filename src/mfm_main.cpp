#include "timers.h"
#include "core/basic_types.h"
#include "core/log.h"
#include "core/maths.h"
#include "wrap/input_wrap.h"
#include "core/shader_loader.h"
#include "core/dir.h"
#include "core/gpu_timer.h"
#include "core/cpu_timer.h"

#include "shaders/cpu_gpu_shared.inl"
#include "shaders/defines.inl" // for RADIUS
//#include "shaders/splitmix32.inl"
#include "splat_compiler.h"

#include <GL/gl3w.h>
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>

#define PRNG_STATE_FORMAT  GL_RGBA32UI
#define EVENT_COUNT_FORMAT GL_R32UI
#define VOTE_FORMAT        GL_R32UI
#define SITE_FORMAT        GL_RGBA32UI
#define COLOR_FORMAT       GL_RGBA8
#define DEV_FORMAT         GL_RGBA32UI

#define NO_SITE ivec2(-1);

// mfm state
static ivec2 world_res = ivec2(0);
static ivec2 vote_res = ivec2(0);
static GLuint site_bits_img = 0;
static GLuint color_img = 0;
static GLuint dev_img = 0;
static GLuint vote_img = 0;
static GLuint event_count_img = 0;
static GLuint prng_state_img = 0;
static GLuint event_job_handles[2];
static GLuint command_handles[2];
static int event_job_write_idx = 0;
static int event_job_count_max = 0;
static u32 dispatch_counter = 0;
static bool reset_ok = false;
static ProgramInfo prog_info;

// stats state
#define STATS_BUFFER_SIZE 2
static GLuint stats_handles[STATS_BUFFER_SIZE];
static WorldStats stats;
static GLuint site_info_handles[STATS_BUFFER_SIZE];
static SiteInfo site_info;
static int stats_write_idx = 0;
static ivec2 site_info_idx = ivec2(0);
static bool hold_site_info_idx = false;


// render state
static GLuint unit_quad_vao;
static GLuint unit_quad_vbo;
static GLuint unit_quad_ebo;
//static bool do_update = false;
static bool run = false;
static int update_rate_custom = 16;
static int update_rate_option = 1;
static pose camera_from_world = identity();
static pose camera_from_world_start = identity();
static pose camera_from_world_target = identity();

// gui state
static ivec2 gui_world_res_prev = ivec2(0); // needs to be 0 to trigger first frame reset
static ivec2 gui_world_res = ivec2(256);
static int gui_res_option = 7;
static GPUTimer update_timer;
static bool open_shader_gui = false;
static s64 time_of_reset = 0;
static s64 events_since_reset = 0;
static double sim_time_since_reset = 0.0;
static double wall_time_since_reset = 0.0;
static int gui_stop_at_n_dispatches = 800;
static bool stop_update_on_n_dispatches = false;
static float scale_before_zoomout = 1.0f;
static bool is_overview = false;
static float mouse_wheel_smooth = 0.0f;
static int overview_state = 0;
static float overview_transition_timer = -1.0f;
static float AER_history[30];
static u32 AER_history_idx = 0;


static void drawViewport(int llx, int lly, int width, int height) {
	glViewport(llx, lly, width, height);
}
static void drawClear(float r, float g, float b) {
	glClearColor(r, g, b, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
}

static u32 renderGetUnitQuadVao() {
	if (unit_quad_vao != 0) return unit_quad_vao;
	// create render geometry
	glGenVertexArrays(1, &unit_quad_vao);
	glGenBuffers(1, &unit_quad_vbo);
	glGenBuffers(1, &unit_quad_ebo);

	// Bind the Vertex Array Object first, then bind and set vertex buffer(s) and attribute pointer(s).
	glBindVertexArray(unit_quad_vao);

	// cover the screen, but match pixels to aspect ratio
	static const float verts[] = { 0,0, 0,0,   1,0, 1,0,   0,1, 0,1,  1,1, 1,1, };

	glBindBuffer(GL_ARRAY_BUFFER, unit_quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	static const unsigned int elems[] = { 0,1,2, 1,3,2 };
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, unit_quad_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elems), (void*)elems, GL_STATIC_DRAW);

	int stride = 4 * sizeof(float);
	// position
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
	glEnableVertexAttribArray(0);

	// uv
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(0 + 2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	return unit_quad_vao;
}
static void texParamsClampToZeroAndNearest() {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); // 0 by default
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
}
static bool resizeWorldIfNeeded(ivec2 req_world_res) {
	ivec2 req_vote_res = req_world_res + ivec2(EVENT_WINDOW_RADIUS * 2 * 2); // edges need to see R*2 off the edge, on each border

	if (site_bits_img == 0) glGenTextures(1, &site_bits_img);
	if (color_img == 0) glGenTextures(1, &color_img);
	if (dev_img == 0) glGenTextures(1, &dev_img);
	if (vote_img == 0) glGenTextures(1, &vote_img);
	if (event_count_img == 0) glGenTextures(1, &event_count_img);
	if (prng_state_img == 0) glGenTextures(1, &prng_state_img);

	for (unsigned i = 0; i < ARRSIZE(event_job_handles); ++i) 
		if (event_job_handles[i] == 0)
			glGenBuffers(1, &event_job_handles[i]);
	for (unsigned i = 0; i < ARRSIZE(command_handles); ++i) 
		if (command_handles[i] == 0)
			glGenBuffers(1, &command_handles[i]);

	if (world_res != req_world_res) {
		glBindTexture(GL_TEXTURE_2D, site_bits_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, SITE_FORMAT, req_world_res.x, req_world_res.y, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindTexture(GL_TEXTURE_2D, color_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, COLOR_FORMAT, req_world_res.x, req_world_res.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindTexture(GL_TEXTURE_2D, dev_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, DEV_FORMAT, req_world_res.x, req_world_res.y, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindTexture(GL_TEXTURE_2D, vote_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, VOTE_FORMAT, req_vote_res.x, req_vote_res.y, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindTexture(GL_TEXTURE_2D, event_count_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, EVENT_COUNT_FORMAT, req_world_res.x, req_world_res.y, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindTexture(GL_TEXTURE_2D, prng_state_img);
		texParamsClampToZeroAndNearest();
		glTexImage2D(GL_TEXTURE_2D, 0, PRNG_STATE_FORMAT, req_vote_res.x, req_vote_res.y, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);


		event_job_count_max = int(floor(double(req_world_res.x * req_world_res.y) * 0.008)); // seems to be about 0.007 on avg
		for (unsigned i = 0; i < ARRSIZE(event_job_handles); ++i) {
			glBindBuffer(GL_ARRAY_BUFFER, event_job_handles[i]);
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(event_job_count_max * sizeof(EventJob)), 0, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		}

		// replace with GL_DISPATCH_INDIRECT_BUFFER
		IndirectCommand command;
		command.firstIndex = 0;
		command.count = 6;
		command.primCount = 0; 
		command.baseVertex = 0;
		command.baseInstance = 0;
		for (unsigned i = 0; i < ARRSIZE(command_handles); ++i) { 
			glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_handles[i]);
			glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(IndirectCommand), &command, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
		}

		world_res = req_world_res;
		vote_res = req_vote_res;
		return true;
	}
	return false;
}
static void initStatsIfNeeded() {
	if (stats_handles[0] == 0) {
		WorldStats zero_stats;
		memset(&zero_stats, 0, sizeof(WorldStats));
		for (unsigned i = 0; i < ARRSIZE(stats_handles); i++) {
			glGenBuffers(1, &stats_handles[i]);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, stats_handles[i]);
			glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof(WorldStats)), &zero_stats, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		}
	}
	if (site_info_handles[0] == 0) {
		SiteInfo zero_info;
		memset(&zero_info, 0, sizeof(SiteInfo));
		for (unsigned i = 0; i < ARRSIZE(site_info_handles); i++) {
			glGenBuffers(1, &site_info_handles[i]);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, site_info_handles[i]);
			glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof(SiteInfo)), &zero_info, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		}
	}
}
static void mfmSetUniforms(int stage = 0, GLuint event_job_handle = 0, GLuint command_handle = 0) {
	glBindImageTexture(0, site_bits_img, 0, GL_FALSE, 0, GL_READ_WRITE, SITE_FORMAT);
	glUniform1i(0, 0);
	glBindImageTexture(1, color_img, 0, GL_FALSE, 0, GL_WRITE_ONLY, COLOR_FORMAT);
	glUniform1i(1, 1);
	glBindImageTexture(2, dev_img, 0, GL_FALSE, 0, GL_READ_WRITE, DEV_FORMAT);
	glUniform1i(2, 2);
	glBindImageTexture(3, vote_img, 0, GL_FALSE, 0, GL_READ_WRITE, VOTE_FORMAT);
	glUniform1i(3, 3);
	glBindImageTexture(4, event_count_img, 0, GL_FALSE, 0, GL_READ_WRITE, EVENT_COUNT_FORMAT);
	glUniform1i(4, 4);
	glBindImageTexture(5, prng_state_img, 0, GL_FALSE, 0, GL_READ_WRITE, PRNG_STATE_FORMAT);
	glUniform1i(5, 5);
	glUniform1ui(6, dispatch_counter);
	glUniform2i(7, site_info_idx.x, site_info_idx.y);
	glUniform1i(8, stage);
	glUniform1ui(9, event_job_count_max);
	glUniform1i(10, 0); // hacky constants to prevent gpu loop unrolling
	glUniform1i(11, 1);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, stats_handles[stats_write_idx]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, site_info_handles[stats_write_idx]);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, event_job_handle);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, command_handle);
}
static bool mfmGPUReset(ivec2 world_res) {
	if (useProgram("shaders/reset.comp")) {
		mfmSetUniforms();
		glMemoryBarrier(GL_ALL_BARRIER_BITS);//GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		glDispatchCompute((vote_res.x / GROUP_SIZE_X) + 1, (vote_res.y / GROUP_SIZE_Y) + 1, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
		glMemoryBarrier(GL_ALL_BARRIER_BITS);//GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
	time_of_reset = time_counter();
	return !checkForShaderErrors();
}
static bool mfmComputeStats(ivec2 world_res) {
	if (useProgram("shaders/stats.comp")) {
		mfmSetUniforms();
		gtimer_start("stats");
		glDispatchCompute((world_res.x / GROUP_SIZE_X) + 1, (world_res.y / GROUP_SIZE_Y) + 1, 1);
		gtimer_stop();
		glMemoryBarrier(
			GL_SHADER_STORAGE_BARRIER_BIT      |
			GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
			GL_TEXTURE_FETCH_BARRIER_BIT          // probably not needed
		);
	}

	return !checkForShaderErrors();
}
static bool mfmClearStats() {
	if (useProgram("shaders/clear_stats.comp")) {
		mfmSetUniforms();
		gtimer_start("clear stats");
		glDispatchCompute(1, 1, 1);
		//glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT); // opt
		glMemoryBarrier(GL_ALL_BARRIER_BITS); // safe
		gtimer_stop();
	}
	return !checkForShaderErrors();
}
static void mfmReadStats() {
	gtimer_start("read stats");
	int stats_read_idx = 1 - stats_write_idx;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, stats_handles[stats_read_idx]);
	glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(WorldStats), &stats);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, site_info_handles[stats_read_idx]);
	glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(SiteInfo), &site_info);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	events_since_reset += stats.event_count_this_batch;
	stats_write_idx = stats_read_idx;
	gtimer_stop();
}
static bool mfmGPUUpdate(ivec2 world_res, int dispatches_per_batch, int stop_at_n_dispatches) {
	if (dispatches_per_batch == 0) return true;
	update_timer.reset();
	update_timer.start("batch");
	gtimer_start("update");

	if (useProgram("shaders/staged_update_direct.comp")) {
		int event_job_handle = event_job_handles[event_job_write_idx];
		int command_handle = command_handles[event_job_write_idx];
		for (int i = 0; i < dispatches_per_batch; ++i) { 
			if (stop_at_n_dispatches != 0 && dispatch_counter == stop_at_n_dispatches) break;

			mfmSetUniforms(0, event_job_handle, command_handle);
			gtimer_start("vote");
			glDispatchCompute((vote_res.x / GROUP_SIZE_X) + 1, (vote_res.y / GROUP_SIZE_Y) + 1, 1);
			//glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT); // opt
			glMemoryBarrier(GL_ALL_BARRIER_BITS); // safe
			gtimer_stop();
		
			mfmSetUniforms(1, event_job_handle, command_handle);
			gtimer_start("tick");
			glDispatchCompute((world_res.x/GROUP_SIZE_X)+1, (world_res.y/GROUP_SIZE_Y)+1,1);
			//glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT); // opt
			//glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT); // opt, get ready to render
			glMemoryBarrier(GL_ALL_BARRIER_BITS); // safe
			gtimer_stop();

			dispatch_counter++;
		}
	}

	update_timer.stop();
	gtimer_stop();

	return !checkForShaderErrors();
}


void mfmInit(float aspect) {

}
void mfmTerm() {

}

static bool mfmGPURender(ivec2 world_res) {
	if (useProgram("shaders/render.comp")) {
		gtimer_start("render");
		mfmSetUniforms();
		glDispatchCompute((world_res.x/GROUP_SIZE_X)+1, (world_res.y/GROUP_SIZE_Y)+1,1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
		gtimer_stop();
	}
	return !checkForShaderErrors();
}
bool mfmDraw(ivec2 screen_res, ivec2 world_res, pose camera_from_world) {
	gtimer_start("draw");
	float s = float(screen_res.y) / float(world_res.y);
	ivec2 dims = s > 1.0f ? world_res * ivec2(s) : world_res / ivec2(1 + 1.0f/s);
	ivec2 pos = screen_res/ivec2(2) - dims/ivec2(2);
	//glViewport(pos.x,pos.y,dims.x,dims.y);
	glViewport(0,0, screen_res.x, screen_res.y);
	if (useProgram("shaders/unit_quad.vert", "shaders/draw.frag")) {
		glEnable(GL_FRAMEBUFFER_SRGB);
		glActiveTexture(GL_TEXTURE0 + 0);
		glBindTexture(GL_TEXTURE_2D, site_bits_img);
		glUniform1i(2, 0);

		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, dev_img);
		glUniform1i(3, 1);

		glActiveTexture(GL_TEXTURE0 + 2);
		glBindTexture(GL_TEXTURE_2D, color_img);
		glUniform1i(4, 2);

		glActiveTexture(GL_TEXTURE0 + 3);
		glBindTexture(GL_TEXTURE_2D, vote_img);
		glUniform1i(5, 3);

		
		float camera_aspect = float(screen_res.x) / float(screen_res.y);
		float world_aspect = float(world_res.x) / float(world_res.y);
		vec2 pos = camera_from_world.xy();
		vec2 scale = vec2(world_aspect, 1.0f) * vec2(world_res.y) * scaleof(camera_from_world);
		glUniform2f(20, pos.x, pos.y);
		glUniform2f(21, scale.x, scale.y);
		glUniform1f(22, 1.0f / camera_aspect);

		glBindVertexArray(renderGetUnitQuadVao());

		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
		glBindVertexArray(0);

		glActiveTexture(GL_TEXTURE0 + 3);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0 + 2);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0 + 0);
		glBindTexture(GL_TEXTURE_2D, 0);

		glDisable(GL_FRAMEBUFFER_SRGB);
	}
	gtimer_stop();
	return !checkForShaderErrors();
}

static void dirscanCallback(const char* pathfile, const char* name, const char* ext) {
	gui::Selectable(name, false);
} 
static pose calcOverviewPose(ivec2 world_res) {
	vec2 w = vec2(world_res);
	float s = 2.0f / w.y * 0.9f; // "a little bit zoomed out"
	return pose(-w.x * 0.5 * s, -w.y * 0.5 * s , s, 0.0f);
}
static pose zoomIncrementalCenteredOnPoint(pose camera_from_world, vec2 camera_from_point, float zoom_amount) {
	float curr_scale = scaleof(camera_from_world);
	float new_scale = exp(log(curr_scale) + zoom_amount);
	return trans(camera_from_point) * scale(new_scale / curr_scale) * trans(-camera_from_point);
}
static pose zoomInstantCenteredOnPoint(pose camera_from_world, vec2 camera_from_point, float new_scale) {
	float curr_scale = scaleof(camera_from_world);
	return trans(camera_from_point) * scale(new_scale / curr_scale) * trans(-camera_from_point) *  camera_from_world;
}

struct GuiSettings {
	bool enable_break_at_step = false;
	int break_at_step_number = 1000;
	int run_speed = 1;
	int size_option = 7;
	ivec2 size_custom = ivec2(80, 60);
};

static GuiSettings gui_set;

static void guiControl(bool* reset, bool* run, bool* step, ivec2* size_request, int dispatch_counter, float AEPS, float AER) {
	bool size_change = false;
	gui::Text("Size:");
	size_change |= gui::RadioButton("64x64     ", &gui_set.size_option,  6); gui::SameLine();
	size_change |= gui::RadioButton("128x128   ", &gui_set.size_option,  7); gui::SameLine();
	size_change |= gui::RadioButton("256x256   ", &gui_set.size_option,  8); gui::SameLine();
	size_change |= gui::RadioButton("512x512   ", &gui_set.size_option,  9); 
	size_change |= gui::RadioButton("1024x1024 ", &gui_set.size_option, 10); gui::SameLine();
	size_change |= gui::RadioButton("2048x2048 ", &gui_set.size_option, 11); gui::SameLine();
	size_change |= gui::RadioButton("4096x4096 ", &gui_set.size_option, 12); gui::SameLine();
	size_change |= gui::RadioButton("16384x8192", &gui_set.size_option, 14);
	size_change |= gui::RadioButton("Custom:",     &gui_set.size_option, 0); gui::SameLine();
	size_change |= gui::InputInt2("##size_custom", (int*)&gui_set.size_custom);
	if (gui_set.size_option != 0) {
		*size_request = ivec2(1 << gui_set.size_option);
	}
	else {
		if (gui_set.size_custom.x > 0 && gui_set.size_custom.y > 0)
			*size_request = gui_set.size_custom;
	}
	size_request->y = min(size_request->y, 8192);

	if (size_change) {
		//*run = false;
		*reset = true;
	}
	
	if (gui_set.enable_break_at_step && dispatch_counter == (unsigned)gui_set.break_at_step_number) 
		*run = false;

	gui::Separator();
	
	gui::Columns(2, 0, true);
	gui::SetColumnWidth(0, 300.0f);
	gui::SetCursorPosX(30);
	if (gui::Button("Reset and Run")) {
		*reset = true;
		*run = true;
	} gui::SameLine();
	if (gui::Button("Reset")) {
		*reset = true;
		*run = false;
	} gui::SameLine();

	if (*run) {
		if (gui::Button("Break")) {
			*run = false;
		}
	} else {
		if (gui::Button(" Run ")) {
			*run = true;
		}
	}
	gui::SameLine();

	// #SUBTLE don't want holding down Step button to blow past break counter
	if (!gui_set.enable_break_at_step || dispatch_counter != (unsigned)gui_set.break_at_step_number-1)
		gui::PushButtonRepeat(true);
	if (gui::Button("Step")) {
		*step = true;
		*run = false;
		if (dispatch_counter == (unsigned)gui_set.break_at_step_number)
			gui_set.enable_break_at_step = false;
	}
	if (!gui_set.enable_break_at_step || dispatch_counter != (unsigned)gui_set.break_at_step_number-1)
		gui::PopButtonRepeat();

	
	gui::AlignFirstTextHeightToWidgets();
	gui::Text("Speed:"); gui::SameLine();
	gui::SetCursorPosX(55);
	gui::RadioButton("1  ",   &gui_set.run_speed, 1); gui::SameLine();
	gui::RadioButton("2  ",   &gui_set.run_speed, 2); gui::SameLine();
	gui::RadioButton("4  ",   &gui_set.run_speed, 4); gui::SameLine();
	gui::RadioButton("8  ",   &gui_set.run_speed, 8); gui::SameLine();
	gui::RadioButton("16 ",  &gui_set.run_speed, 16);
	gui::SetCursorPosX(55);
	gui::RadioButton("32 ",  &gui_set.run_speed, 32); gui::SameLine();
	gui::RadioButton("64 ",  &gui_set.run_speed, 64); gui::SameLine();
	gui::RadioButton("128", &gui_set.run_speed, 128); gui::SameLine();
	gui::RadioButton("256", &gui_set.run_speed, 256);

	if (gui_set.enable_break_at_step) {
		gui::Checkbox("Break at step number:", &gui_set.enable_break_at_step); gui::SameLine();
		gui::PushItemWidth(100.0f);
		gui::InputInt("##break_at_step_number", &gui_set.break_at_step_number);
		gui::PopItemWidth();
		gui::SameLine();
		//gui::Text("Current: %5d", dispatch_counter);
	}
	else {
		gui::Checkbox("Break at step...", &gui_set.enable_break_at_step); gui::SameLine();
	}

	gui_set.break_at_step_number = max(1, gui_set.break_at_step_number);

	gui::NextColumn();

	if (AEPS < 1000.0)
		gui::Text("AEPS: %.1f", AEPS);
	else
		gui::Text("AEPS: %.3fK", AEPS/1000.0);
	if (gui::IsItemHovered()) gui::SetTooltip("Average Events Per Site");
	
	gui::Text("AER:  %0.3f", AER);
	if (gui::IsItemHovered()) gui::SetTooltip("Average Event Rate (AEPS/sec)");
	
	gui::Text("Step: %d", dispatch_counter);
	if (gui::IsItemHovered()) gui::SetTooltip("Steps since last reset");

	gui::Columns();

}
void mfmUpdate(Input* main_in, int app_res_x, int app_res_y, int refresh_rate) {
	ivec2 screen_res = ivec2(app_res_x, app_res_y);
	

	bool do_reset = false;
	bool do_step = false;

	bool mouse_is_onscreen = main_in->mouse.x >= 0 && main_in->mouse.x <= 1.0f && main_in->mouse.y >= 0 && main_in->mouse.y <= 1.0f; 
	bool mouse_is_using_ui = gui::IsMouseHoveringAnyWindow() || gui::IsAnyItemActive();
	float camera_aspect = float(app_res_x) / float(app_res_y);
	vec2 camera_from_mouse = (vec2(main_in->mouse.x, main_in->mouse.y)*2.0f - vec2(1.f)) * vec2(camera_aspect, 1.f);
	vec2 world_from_mouse = ~camera_from_world * camera_from_mouse;
	ivec2 hover_site_idx = ivec2(world_from_mouse);
	hover_site_idx.y = gui_world_res.y - 1 - hover_site_idx.y; //#Y-DOWN

#define OVERVIEW_INACTIVE 0
#define OVERVIEW_ZOOMING_IN 1
#define OVERVIEW_ZOOMING_OUT 2

	mouse_wheel_smooth = lerp(mouse_wheel_smooth, main_in->mouse.wheel, 0.2f);
	pose mouse_zoom_diff = identity();
	pose mouse_pan_diff = identity();
	if (mouse_is_onscreen && !mouse_is_using_ui) {
		mouse_zoom_diff = zoomIncrementalCenteredOnPoint(camera_from_world_target, camera_from_mouse, mouse_wheel_smooth * 0.1f);
			
		if (main_in->mouse.down[MOUSE_BUTTON_2]) {
			mouse_pan_diff = trans(vec2(main_in->mouse.dx, main_in->mouse.dy) * vec2(camera_aspect, 1.f) * 2.0f);
		}
		if (main_in->mouse.press[MOUSE_BUTTON_3]) { 
			pose camera_from_world_overview = calcOverviewPose(gui_world_res);
			pose camera_from_world_focus = zoomInstantCenteredOnPoint(camera_from_world, camera_from_mouse, scale_before_zoomout);
			camera_from_world_start = camera_from_world;
			overview_transition_timer = 0.0f;
			if (overview_state == OVERVIEW_INACTIVE) {
				float scale_diff = exp(abs(log(scaleof(camera_from_world)) - log(scaleof(camera_from_world_overview))));
				if (scale_diff >= 1.01f) { // if we're not near overview scale, go to overview
					scale_before_zoomout = scaleof(camera_from_world); 
					camera_from_world_target = camera_from_world_overview;
					overview_state = OVERVIEW_ZOOMING_OUT;
				} else {
					camera_from_world_target = camera_from_world_focus;
					overview_state = OVERVIEW_ZOOMING_IN;
				}
			} else if (overview_state == OVERVIEW_ZOOMING_IN) {
				camera_from_world_target = camera_from_world_overview;
				overview_state = OVERVIEW_ZOOMING_OUT;
			} else { // zooming out
				camera_from_world_target = camera_from_world_focus;
				overview_state = OVERVIEW_ZOOMING_IN;
			}
		}
		if (hover_site_idx >= ivec2(0,0) && hover_site_idx < gui_world_res) {
			if (main_in->mouse.press[MOUSE_BUTTON_1])
				hold_site_info_idx = !hold_site_info_idx;
		}
	}

	if (!hold_site_info_idx)
		site_info_idx = hover_site_idx;
	
	const float OVERVIEW_TRANSITION_TIME = 0.5f;
	if (overview_transition_timer >= 0.0f) {
		float a = clamp(overview_transition_timer / OVERVIEW_TRANSITION_TIME, 0.0f, 1.0f);
		camera_from_world = lerp(camera_from_world_start, camera_from_world_target, smoothstep(a));
		if (a == 1.0f) {
			overview_transition_timer = -1.0f;
			overview_state = OVERVIEW_INACTIVE;
		}
		else
			overview_transition_timer += 1.0f / 60.0f;
	} else {
		camera_from_world = mouse_zoom_diff * camera_from_world;
		camera_from_world = mouse_pan_diff * camera_from_world;
	}

	static s64 time_of_frame_start = 0;
	float sec_per_frame = run ? time_to_sec(time_counter() - time_of_frame_start) : 0.0f;
	time_of_frame_start = time_counter();

	GPUCounterFrame* gpu_frame = update_timer.read();
	float sec_per_batch = getTimeOfLabel(gpu_frame, "batch");
	if (run) {
		sim_time_since_reset += sec_per_batch;
		wall_time_since_reset += sec_per_frame;
	}

	int dispatch_count = 0;

	// compute AEPS and AER
	int total_sites = gui_world_res.x * gui_world_res.y;
	double AEPS = double(events_since_reset) / double(total_sites);
	float events_per_sec = stats.event_count_this_batch / sec_per_batch;
	float AER = sec_per_batch == 0.0f ? 0.0f : events_per_sec / total_sites;
	float AER_avg = 0.0f;
	for (int i = 0; i < ARRSIZE(AER_history); ++i)
		AER_avg += AER_history[i];
	AER_avg /= ARRSIZE(AER_history);
	
	if (gui::Begin("Control")) {
		guiControl(&do_reset, &run, &do_step, &gui_world_res, dispatch_counter, AEPS, AER_avg);
	} gui::End();

	if (run) {
		dispatch_count = gui_set.run_speed;
	} else if (do_step) {
		dispatch_count = 1;
	}

	if (do_reset)
		memset(AER_history, 0, sizeof(AER_history));
	if (run)
		AER_history[(AER_history_idx++)%ARRSIZE(AER_history)] = AER;
	
	if (gui_world_res != gui_world_res_prev)
		do_reset = true;

	int stop_at_n_dispatches = gui_set.enable_break_at_step ? gui_set.break_at_step_number : 0;

	if (gui::Begin("Performance")) {
		float sec_per_dispatch = dispatch_count == 0 ? 0.0f : sec_per_batch / dispatch_count;
		float events_per_sim_sec = stats.event_count_this_batch / sec_per_batch;
		float events_per_wall_sec = sec_per_frame == 0.0f ? 0.0f : stats.event_count_this_batch / sec_per_frame;
		float event_count_this_dispatch = stats.event_count_this_batch / (dispatch_count == 0 ? 1 : dispatch_count);
		float AER = events_per_sim_sec / total_sites;
		float WAER = events_per_wall_sec / total_sites;
		gui::Text("Events per second:  %3.3fM",    events_per_sim_sec / (1000000.0f)); 
		gui::Text("Batch:              %3.3f ms",  sec_per_batch * 1000.0f);
		gui::Text("Dispatch:           %3.3f ms",  sec_per_dispatch * 1000.0f);
		gui::Text("AER:                %3.3f",     AER);
		gui::Text("WAER:               %3.3f (%.1f%%)", WAER, WAER/(AER == 0.0f ? 1.0f : AER)*100.0f);
		gui::Text("Sites updated:      %.0f (%3.3f%%)", event_count_this_dispatch, event_count_this_dispatch / float(total_sites) * 100.0f);
		gui::Text("Time:               %3.1f sec", sim_time_since_reset);
	} gui::End(); 

	/*
	if (gui::Begin("Site Inspector")) {
		static int inspect_mode = INSPECT_MODE_BASIC;
		gui::RadioButton("basic", &inspect_mode, INSPECT_MODE_BASIC); gui::SameLine();
		gui::RadioButton("full", &inspect_mode, INSPECT_MODE_FULL);
		gui::Spacing();
		gui::Text("Site: (%d, %d)", site_info_idx.x, site_info_idx.y);
		if (inspect_mode == INSPECT_MODE_FULL)
			gui::Text("dev: %d %d %d %d", site_info.dev.x, site_info.dev.y, site_info.dev.z, site_info.dev.w);
		guiAtomInspector(site_info.event_layer, inspect_mode);
	} gui::End();
	*/
	
	drawViewport(0, 0, app_res_x, app_res_y);
	drawClear(21/255.f, 33/255.f, 54/255.f);

	bool world_hash_changed = resizeWorldIfNeeded(gui_world_res);
	initStatsIfNeeded();
	bool file_change = false;
	bool project_change = false;
	
	checkForSplatProgramChanges(&file_change, &project_change, &prog_info);
	do_reset |= project_change;

	if (world_hash_changed) { 
		camera_from_world = camera_from_world_start = camera_from_world_target = calcOverviewPose(gui_world_res);
		scale_before_zoomout = scaleof(camera_from_world);
		overview_state = OVERVIEW_INACTIVE;
	}

	bool update_ok = dispatch_count == 0; // ok if we don't want to update
	bool clear_stats_ok = false;
	bool compute_stats_ok = false;
	bool render_ok = false;
	bool draw_ok = false;

	// update loop
	{
		ctimer_start("update"); 

		if (do_reset) {
			dispatch_counter = 0;
			events_since_reset = 0;
			sim_time_since_reset = 0.0;
			wall_time_since_reset = 0.0;
		}
		if (do_reset)
			reset_ok = mfmGPUReset(gui_world_res);

		clear_stats_ok = mfmClearStats();

		if (reset_ok)
			update_ok = mfmGPUUpdate(gui_world_res, dispatch_count, stop_at_n_dispatches);
	
		compute_stats_ok = mfmComputeStats(gui_world_res);

		if (update_ok)
			render_ok = mfmGPURender(gui_world_res);

		if (clear_stats_ok && compute_stats_ok)
			mfmReadStats();

		ctimer_stop();
	}
	
	// draw loop
	{
		ctimer_start("draw");
		if (render_ok)
			draw_ok = mfmDraw(screen_res, gui_world_res, camera_from_world);
		ctimer_stop();
	}

	if (gui::Begin("Statistics")) {
		gui::Text("Atom Counts:");
		for (int i = 0; i < TYPE_COUNTS; ++i)
			gui::Text("%d: %d (%3.3f%%)", i, stats.counts[i], float(stats.counts[i]) / float(total_sites) * 100.0f);
		gui::End();
	}

	open_shader_gui |= true;
	open_shader_gui |= !reset_ok || !update_ok || !clear_stats_ok || !compute_stats_ok || !draw_ok || !render_ok;
	if (open_shader_gui)
		guiShader(&open_shader_gui);

	gui_world_res_prev = gui_world_res;
}

