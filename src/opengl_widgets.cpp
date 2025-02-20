#include "opengl_widgets.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define STB_IMAGE_IMPLEMENTATION
#include "dmsc/glm_include.hpp"
#include "opengl_toolkit.hpp"
#include "stb_image.h"
#include <algorithm>
#include <iostream>

namespace dmsc {

using OpenGLPrimitives::GLBuffer;
using OpenGLPrimitives::Object;
using OpenGLPrimitives::ObjectInfo;
using OpenGLPrimitives::VertexData;
using namespace tools;

// ------------------------------------------------------------------------------------------------

OpenGLWidget::OpenGLWidget() { init(); }

// ------------------------------------------------------------------------------------------------

OpenGLWidget::~OpenGLWidget() { destroy(); }

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::init() {
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

    const char* glsl_version = "#version 420";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    // Create window with graphics context
    window = glfwCreateWindow(1280, 720, "Dynamic Minimum Scan Cover - Visualizer", NULL, NULL);
    if (window == NULL)
        return;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Resize event
    glfwSetWindowSizeCallback(window,
                              [](GLFWwindow* window, int width, int height) { glViewport(0, 0, width, height); });
    // Mouse wheel event
    glfwSetWindowUserPointer(window, this);
    glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) {
        OpenGLWidget* p = (OpenGLWidget*)glfwGetWindowUserPointer(window);

        if (glfwGetWindowAttrib(window, GLFW_HOVERED) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
            !ImGui::IsAnyItemHovered()) {               // ignore if the mouse points to a UI element
            float zoom_per_deg = .03f * p->zoom * 2.5f; // depends on current zoom value to avoid endless scrolling for
                                                        // high zoom values and too big jumps for small zoom values
            p->zoom += static_cast<float>(yoffset) * zoom_per_deg;
        }
    });

    // Mouse click event
    glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mods) {
        OpenGLWidget* p = (OpenGLWidget*)glfwGetWindowUserPointer(window);

        // ignore every input except left mouse button
        if (button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }

        // if clicked inside the visualization (except UI), begin camera movement - if mouse button
        // released (no matter where), stop it
        switch (action) {
        case GLFW_PRESS:
            if (glfwGetWindowAttrib(window, GLFW_HOVERED) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                !ImGui::IsAnyItemHovered()) {
                p->is_mouse_pressed = true;
                double xpos, ypos;
                glfwGetCursorPos(window, &xpos, &ypos);
                p->mouse_start_location = glm::vec2(static_cast<float>(xpos), static_cast<float>(ypos));
            }
            break;
        case GLFW_RELEASE:
            p->camera_rotation_angle += p->camera_rotation_angle_offset;
            p->camera_rotation_angle_offset = glm::vec2(0.f);
            p->camera_rotation_angle.x = std::fmod(p->camera_rotation_angle.x, static_cast<float>(M_PI) * 2.f);
            p->is_mouse_pressed = false;
            break;
        default:
            break;
        }
    });

    // Mouse move event
    glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) {
        OpenGLWidget* p = (OpenGLWidget*)glfwGetWindowUserPointer(window);

        if (p->is_mouse_pressed) {
            glm::vec2 diff = glm::vec2(xpos, ypos) - p->mouse_start_location;
            int screen_width, screen_height;
            glfwGetWindowSize(window, &screen_width, &screen_height);
            // moving the mouse along half the screen size rotates the scene by 90 degrees
            glm::vec2 angle = (static_cast<float>(M_PI) / 2.f) * diff / glm::vec2(screen_width / 2, -screen_height / 2);
            // as long as the mouse button is not released, the changed rotation is applied as an
            // offset
            p->camera_rotation_angle_offset = angle;
        }
    });

    bool err = gladLoadGL() == 0;
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_PRIMITIVE_RESTART);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.5f);
    glPrimitiveRestartIndex(MAX_ELEMENT_ID);

    // OpenGL-Version
    std::cout << "Open GL 4.2 needed. Given: ";
    std::cout << glGetString(GL_VERSION) << std::endl;

    // Read shader programs from files
    GLuint vertex_shader = createShader("shader/basic.vert", GL_VERTEX_SHADER);
    GLuint fragment_shader = createShader("shader/basic.frag", GL_FRAGMENT_SHADER);
    GLuint earth_frag_shader = createShader("shader/earth.frag", GL_FRAGMENT_SHADER);
    GLuint satellite_vert_shader = createShader("shader/satellite.vert", GL_VERTEX_SHADER);
    GLuint shaded_frag_shader = createShader("shader/shaded.frag", GL_FRAGMENT_SHADER);
    basic_program = createProgram(vertex_shader, fragment_shader);
    satellite_prog = createProgram(satellite_vert_shader, fragment_shader);
    earth_prog = createProgram(vertex_shader, earth_frag_shader);
    shaded_prog = createProgram(satellite_vert_shader, shaded_frag_shader);

    // bind uniform vbo to programs
    glGenBuffers(1, &vbo_uniforms);
    GLuint index = glGetUniformBlockIndex(basic_program, "Global");
    glUniformBlockBinding(basic_program, index, 1);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, vbo_uniforms);
    index = glGetUniformBlockIndex(satellite_prog, "Global");
    glUniformBlockBinding(satellite_prog, index, 1);
    index = glGetUniformBlockIndex(earth_prog, "Global");
    glUniformBlockBinding(earth_prog, index, 1);
    index = glGetUniformBlockIndex(shaded_prog, "Global");
    glUniformBlockBinding(shaded_prog, index, 1);

    // create storage buffer
    glGenBuffers(1, &vbo_static);
    glGenBuffers(1, &ibo_static);
    buffer_transformations = GLBuffer<glm::mat4>(GL_DYNAMIC_DRAW);
    buffer_transformations.gen();
    buffer_satellite_color = GLBuffer<glm::vec3>(GL_DYNAMIC_DRAW);
    buffer_satellite_color.gen();
    buffer_lines = GLBuffer<VertexData>(GL_DYNAMIC_DRAW);
    buffer_lines.gen();

    // create vao
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_static);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_static);
    glEnableVertexAttribArray(0); // vertices
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), 0);
    glEnableVertexAttribArray(1); // colors
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 3));
    glEnableVertexAttribArray(2); // texture
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 7));
    glEnableVertexAttribArray(3); // normals
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 9));
    glBindBuffer(GL_ARRAY_BUFFER, buffer_transformations.buffer_idx); // object transformations
    glEnableVertexAttribArray(4);
    glEnableVertexAttribArray(5);
    glEnableVertexAttribArray(6);
    glEnableVertexAttribArray(7);
    // maximum size for vertexAttr is 4. So we split the 4x4matrix into 4x vec4
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(0));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 4));
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 8));
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 12));
    glVertexAttribDivisor(4, 1); // update transformation attr. every 1 instance instead of every vertex
    glVertexAttribDivisor(5, 1);
    glVertexAttribDivisor(6, 1);
    glVertexAttribDivisor(7, 1);
    glBindVertexArray(0);

    // create vao for satellites
    glGenVertexArrays(1, &vao_satellites);
    glBindVertexArray(vao_satellites);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_static);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_static);
    glEnableVertexAttribArray(0); // vertices
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), 0);
    glEnableVertexAttribArray(1); // colors
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 3));
    glEnableVertexAttribArray(2); // texture
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 7));
    glEnableVertexAttribArray(3); // normals
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 9));
    glBindBuffer(GL_ARRAY_BUFFER, buffer_transformations.buffer_idx); // object transformations
    glEnableVertexAttribArray(4);
    glEnableVertexAttribArray(5);
    glEnableVertexAttribArray(6);
    glEnableVertexAttribArray(7);
    // maximum size for vertexAttr is 4. So we split the 4x4matrix into 4x vec4
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(0));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 4));
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 8));
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 16, (void*)(sizeof(float) * 12));
    glVertexAttribDivisor(4, 1); // update transformation attr. every 1 instance instead of every vertex
    glVertexAttribDivisor(5, 1);
    glVertexAttribDivisor(6, 1);
    glVertexAttribDivisor(7, 1);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_satellite_color.buffer_idx); // dynamic satellite color
    glBufferData(GL_ARRAY_BUFFER, 0, 0,
                 GL_STREAM_DRAW); // push data
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), 0);
    glVertexAttribDivisor(8, 1);
    glBindVertexArray(0);

    // create vao for lines
    glGenVertexArrays(1, &vao_lines);
    glBindVertexArray(vao_lines);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_lines.buffer_idx);
    glEnableVertexAttribArray(0); // vertices
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), 0);
    glEnableVertexAttribArray(1); // colors
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)(sizeof(GL_FLOAT) * 3));
    glBindVertexArray(0);

    // load textures
    loadTextures("earth_day", "textures/earth_day.jpg", texture_id[0]);
    loadTextures("earth_water", "textures/earth_water.jpg", texture_id[1]);
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::loadTextures(const char* uniform_name, const char* file, GLuint& id) {
    int w, h;
    int channels;
    unsigned char* image;
    stbi_set_flip_vertically_on_load(1);
    image = stbi_load(file, &w, &h, &channels, STBI_rgb);

    if (image == nullptr) {
        printf("Failed to load image %s\n", file);
        assert(false);
        exit(EXIT_FAILURE);
    }

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB, // internal format
                 w,
                 h,
                 0,
                 GL_RGB, // target format
                 GL_UNSIGNED_BYTE,
                 image);
    stbi_image_free(image);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::show(const PhysicalInstance& instance, const Animation& animation, const float t0) {
    prepareInstance(instance);
    this->animation = animation;
    sim_time = t0;
    openWindow();
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::show(const PhysicalInstance& instance, const float t0) { show(instance, Animation(), t0); }

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::show(const PhysicalInstance& instance, const DmscSolution& solution, const float t0) {
    Animation anim = animateScanCover(instance, solution.scan_cover);
    show(instance, anim, t0);
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::show(const PhysicalInstance& instance, const FreezeTagSolution& solution, const float t0) {
    Animation anim = animateScanCover(instance, solution.scan_cover);

    // build timeline for satellite orientations and edge order
    float scan_time = 0.f;
    std::multimap<float, size_t> edge_order; // which edges are scanned at a given time (multiple edges can be scanned
                                             // at the same time -> multimap)
    for (const auto& scan : solution.scan_cover) {
        float t = scan.second;              // time when edge is scheduled
        scan_time = std::max(scan_time, t); // find time when all edges were scanned
        // fill edge order
        edge_order.insert({t, scan.first});
    }

    // when does the message recieves a satellite
    std::map<size_t, float> satellites_done;
    for (const auto& sat : solution.satellites_with_message) {
        satellites_done[sat] = 0.f;
    }

    for (const auto& it : edge_order) {
        // does one of the satellites already contain the message?
        const InterSatelliteLink& isl = instance.getISLs().at(it.second);
        auto res_1 = satellites_done.find(isl.getV1Idx());
        auto res_2 = satellites_done.find(isl.getV2Idx());

        // if only one satellite carries the message, the message will be transferred
        if (res_1 == satellites_done.end() && res_2 != satellites_done.end()) {
            // sat 2 carries the message
            satellites_done[isl.getV1Idx()] = it.first; // time when sat 1 recieves the message
        } else if (res_1 != satellites_done.end() && res_2 == satellites_done.end()) {
            // sat 1 carries the message
            satellites_done[isl.getV2Idx()] = it.first; // time when sat 2 recieves the message
        }
    }

    // animate satellite color
    for (const auto& sat : satellites_done) {
        anim.addSatelliteAnimation(
            sat.first, sat.second, scan_time, AnimationDetails(true, glm::vec4(0.f, 1.f, 0.f, 1.f)));
    }

    show(instance, anim, t0);
}

// ------------------------------------------------------------------------------------------------

Animation OpenGLWidget::animateScanCover(const PhysicalInstance& instance, const ScanCover& scan_cover) {
    Animation anim;

    std::multimap<float, size_t> edge_order; // which edges are scanned at a given time (multiple edges can be scanned
                                             // at the same time -> multimap)
    float scan_time = 0.f;

    // build timeline for satellite orientations and edge order
    for (const auto& scan : scan_cover) {
        float t = scan.second;              // time when edge is scheduled
        scan_time = std::max(scan_time, t); // find time when all edges were scanned
        const InterSatelliteLink& isl = instance.getISLs().at(scan.first);
        glm::vec3 needed_orientation = isl.getOrientation(t);

        glm::vec3 sat1 = instance.getSatellites()[isl.getV1Idx()].cartesian_coordinates(t);
        glm::vec3 sat2 = instance.getSatellites()[isl.getV2Idx()].cartesian_coordinates(t);
        float distance = glm::length((sat2 - sat1) / real_world_scale);

        // add corresponding events for both satellites where they have to face in the needed
        // direction in order to perform the scan
        bool res_1 = anim.addOrientationAnimation(isl.getV1Idx(), t, OrientationDetails(needed_orientation, distance));
        bool res_2 = anim.addOrientationAnimation(isl.getV2Idx(), t, OrientationDetails(-needed_orientation, distance));

        if (!res_1 || !res_2) {
            printf("The needed orientation for satellites can not be applied at t=%f!\n", t);
        }

        // fill edge order
        edge_order.insert({t, scan.first});
    }

    // animate isl network
    for (size_t i = 0; i < instance.islCount(); i++) {
        // hide ISL-edges that are not part of the scan cover (anymore)
        auto range = scan_cover.equal_range(i);
        // edge is not part of the scan cover -> hide it
        if (range.first == scan_cover.end()) {
            anim.addISLAnimation(i, 0.f, scan_time, AnimationDetails(false));
            continue;
        } else {
            float latest_use = 0.f;
            for (auto it = range.first; it != range.second; ++it) {
                latest_use = std::max(latest_use, it->second);
            }
            anim.addISLAnimation(i, latest_use, scan_time, AnimationDetails(false));
        }
    }

    // change color for the next edges that will be scanned
    std::vector<size_t> next_edges;
    float te = 0.f, t = 0.f;
    for (const auto& it : edge_order) {
        if (it.first == t) { // this edge will also be visible next
            next_edges.push_back(it.second);
        }

        if (it.first > t) { // edge that will be active later
                            // 1. animate edges that came before
            AnimationDetails d(true, glm::vec4(1.0f, .75f, 0.0f, 1.f));
            for (const auto& edge_idx : next_edges) {
                anim.addISLAnimation(edge_idx, te, t, d);
            }
            next_edges.clear();

            // prepare the next edges
            te = t;
            t = it.first;
            next_edges.push_back(it.second);
        }
    }

    // change color for the last edges that will be scanned
    AnimationDetails d(true, glm::vec4(1.0f, .75f, 0.0f, 1.f));
    for (const auto& edge_idx : next_edges) {
        anim.addISLAnimation(edge_idx, te, t, d);
    }

    return anim;
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::openWindow() {
    glm::vec3 clear_color = glm::vec3(0.03f);

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        buildGUI(); // Update gui

        // Prepare rendering
        ImGui::Render(); // State change
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // render new frame
        renderScene();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // swap
        glfwSwapBuffers(window);
    }
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::renderScene() {
    recalculate();

    // for every program (key) a vector with objects is stored
    for (const auto& obj : scene) {
        glUseProgram(obj.gl_program);
        glBindVertexArray(obj.gl_vao);

        if (obj.name == "central_mass") {
            GLint t1 = glGetUniformLocation(earth_prog, "earth_day");
            GLint t2 = glGetUniformLocation(earth_prog, "specularity_map");

            // bind textures
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_id[0]);
            glUniform1i(t1, 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture_id[1]);
            glUniform1i(t2, 1);
        }

        if (obj.enabled) {
            if (obj.drawInstanced) { // instanced rendering
                glDrawElementsInstancedBaseVertexBaseInstance(obj.gl_draw_mode,
                                                              static_cast<GLint>(obj.number_elements),
                                                              obj.gl_element_type,
                                                              (void*)(obj.offset_elements),
                                                              static_cast<GLsizei>(obj.number_instances),
                                                              static_cast<GLint>(obj.base_index),
                                                              static_cast<GLint>(obj.base_instance));
            } else if (obj.number_elements == 0) { // direct rendering with vertices
                glDrawArrays(obj.gl_draw_mode,
                             static_cast<GLint>(obj.offset_vertices),
                             static_cast<GLsizei>(obj.number_vertices));
            } else { // direct rendering with elements
                glDrawElementsBaseVertex(obj.gl_draw_mode,
                                         static_cast<GLint>(obj.number_elements),
                                         obj.gl_element_type,
                                         (void*)(obj.offset_elements),
                                         static_cast<GLint>(obj.base_index));
            }
        }
    }

    glBindVertexArray(0);
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::recalculate() {
    if (state == EMPTY) {
        return;
    }

    // relative time
    float diff = ImGui::GetIO().DeltaTime;
    if (!paused)
        sim_time += diff * sim_speed;

    // sun rotation
    float sun_angle = sim_time * 0.000290f; // 6h -> one turn around the earth
    glm::mat4 sun_rotation = glm::rotate(glm::mat4(1.f), sun_angle, glm::vec3(0.f, 1.f, 0.f));

    /* Camera circumnavigating around the central mass.
     * Instead of performing two rotations for the camera, the rotation around the y-axis is done by
     * rotation the entire world itself. */
    glm::vec2 delta = camera_rotation_angle + camera_rotation_angle_offset;
    // the earth can not be circumnavigated along the poles.
    float max_angle_y = static_cast<float>(M_PI) / 2.f - 0.1f;
    delta.y = glm::clamp(delta.y, -max_angle_y, max_angle_y);
    camera_rotation_angle.y = glm::clamp(camera_rotation_angle.y, -max_angle_y, max_angle_y);

    // calc view matrix
    glm::mat4 camera_rotation = glm::rotate(glm::mat4(1.f), delta.y, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 world_rotation = glm::rotate(glm::mat4(1), delta.x, glm::vec3(0.f, 1.f, 0.f));
    glm::vec4 camera_position = camera_rotation * glm::vec4(camera_init_position, 0.f);
    view = glm::lookAt(glm::vec3(camera_position),
                       glm::vec3(0.0f, 0.0f, 0.0f),
                       glm::vec3(0.0f, 1.0f, 0.0f)); // (position, look at, up)

    // Calc MVP
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    projection = glm::perspective(45.0f, 1.0f * viewport[2] / viewport[3], 0.1f, 10.0f);
    glm::mat4 scale = glm::scale(glm::vec3(zoom));

    // push mvp to VBO
    glBindBuffer(GL_UNIFORM_BUFFER, vbo_uniforms);
    glBufferData(GL_UNIFORM_BUFFER, 5 * sizeof(glm::mat4), 0, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glm::mat4), glm::value_ptr(world_rotation));
    glBufferSubData(GL_UNIFORM_BUFFER, 1 * sizeof(glm::mat4), sizeof(glm::mat4), glm::value_ptr(view));
    glBufferSubData(GL_UNIFORM_BUFFER, 2 * sizeof(glm::mat4), sizeof(glm::mat4), glm::value_ptr(projection));
    glBufferSubData(GL_UNIFORM_BUFFER, 3 * sizeof(glm::mat4), sizeof(glm::mat4), glm::value_ptr(scale));
    glBufferSubData(GL_UNIFORM_BUFFER, 4 * sizeof(glm::mat4), sizeof(glm::mat4), glm::value_ptr(sun_rotation));
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // #############################
    // # dynamic part of scene
    // #############################

    buffer_satellite_color.values.clear();
    buffer_transformations.values.clear();
    recalculateOrbitPositions();
    recalculateLines();

    // the buffer for transfomation and color must match (same instance), so we just fill up the color buffer
    if (buffer_satellite_color.size() < buffer_transformations.size()) {
        buffer_satellite_color.values.insert(buffer_satellite_color.values.end(),
                                             buffer_transformations.size() - buffer_satellite_color.size(),
                                             glm::vec3(-1.f));
    }

    // push data to VBO
    if (buffer_transformations.values.size() != 0) {
        size_t size_transformation = sizeof(buffer_transformations.values[0]) * buffer_transformations.values.size();
        glBindBuffer(GL_ARRAY_BUFFER, buffer_transformations.buffer_idx); // set active
        glBufferData(GL_ARRAY_BUFFER,
                     size_transformation,
                     &buffer_transformations.values[0],
                     GL_STREAM_DRAW); // push data
    }

    if (buffer_satellite_color.values.size() != 0) {
        size_t size_transformation = sizeof(glm::vec3) * buffer_satellite_color.values.size();
        glBindBuffer(GL_ARRAY_BUFFER, buffer_satellite_color.buffer_idx); // set active
        glBufferData(GL_ARRAY_BUFFER,
                     size_transformation,
                     &buffer_satellite_color.values[0],
                     GL_STREAM_DRAW); // push data
    }
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::recalculateOrbitPositions() {
    // move satellites
    glm::mat4 scale = glm::inverse(glm::scale(glm::vec3(zoom))); // ignore zoom

    auto info = getObjectInfo("satellites");
    if (info != nullptr)
        info->base_instance = buffer_transformations.size(); // offset

    // the buffer for transfomation and color must match (same instance), so we just fill up the color buffer
    if (buffer_satellite_color.size() < buffer_transformations.size()) {
        buffer_satellite_color.values.insert(buffer_satellite_color.values.end(),
                                             buffer_transformations.size() - buffer_satellite_color.size(),
                                             glm::vec3(-1.f));
    }

    for (size_t i = 0; i < problem_instance.getSatellites().size(); i++) {
        const Satellite& o = problem_instance.getSatellites()[i];
        glm::vec3 position = o.cartesian_coordinates(sim_time) / real_world_scale;
        glm::mat4 translation = glm::translate(position);

        auto result = animation.getSatelliteAnimation(i, sim_time);
        if (result.first) {
            if (!result.second.visible) {
                translation *= glm::scale(glm::vec3(0.f)); // this satellite has to be invisible rn
            }
            buffer_satellite_color.values.push_back(result.second.color);
        } else {
            buffer_satellite_color.values.push_back(glm::vec3(-1));
        }

        buffer_transformations.values.push_back(translation * scale);
    }
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::recalculateISLNetwork() {
    auto info = getObjectInfo("isl_network");
    if (info == nullptr) {
        printf("Object info for '%s' was not created yet!.\n", "isl_network");
        assert(false);
        exit(EXIT_FAILURE);
    }
    info->offset_vertices = buffer_lines.size();

    Object isl_network;
    for (uint32_t i = 0; i < problem_instance.islCount(); i++) {
        const InterSatelliteLink& edge = problem_instance.getISLs().at(i);
        glm::vec3 sat1 = edge.getV1().cartesian_coordinates(sim_time) / real_world_scale;
        glm::vec3 sat2 = edge.getV2().cartesian_coordinates(sim_time) / real_world_scale;
        glm::vec4 color = glm::vec4(1.f);

        auto result = animation.getISLAnimation(i, sim_time);
        if (result.first) {
            if (!result.second.visible)
                continue; // this isl has to be invisible rn
            color = result.second.color;
        } else {
            if (edge.isBlocked(sim_time)) { // edge can not be scanned
                color = glm::vec4(1.0f, 0.0f, 0.0f, 1.f);
            } else { // edge can be scanned
                color = glm::vec4(0.0f, 1.0f, 0.0f, 1.f);
            }
        }

        Object edge_line = OpenGLPrimitives::createLine(sat1, sat2, color);
        isl_network.add(edge_line);
    }

    info->number_vertices = isl_network.vertexCount();
    buffer_lines.values.insert(buffer_lines.values.end(), isl_network.vertices.begin(), isl_network.vertices.end());
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::recalculateLines() {
    glm::mat4 scale = glm::inverse(glm::scale(glm::vec3(zoom))); // ignore zoom
    buffer_lines.values.clear();

    // build ISL network
    recalculateISLNetwork();

    // build scheduled communications
    auto info = getObjectInfo("scheduled_communications");
    if (info == nullptr) {
        printf("Object info for '%s' was not created yet!.\n", "scheduled_communications");
        assert(false);
        exit(EXIT_FAILURE);
    }
    info->offset_vertices = buffer_lines.size();
    Object scheduled_communications;

    auto info_arrowhead = getObjectInfo("communications_arrowhead");
    if (info_arrowhead != nullptr)
        info_arrowhead->base_instance = buffer_transformations.size();
    for (const auto& c : problem_instance.scheduled_communications) {
        glm::vec3 sat1 = problem_instance.getSatellites()[c.first].cartesian_coordinates(sim_time) / real_world_scale;
        glm::vec3 sat2 = problem_instance.getSatellites()[c.second].cartesian_coordinates(sim_time) / real_world_scale;
        Object communication_line = OpenGLPrimitives::createLine(sat1, sat2, glm::vec4(.55f, .1f, 1.f, 1.f), true);
        scheduled_communications.add(communication_line);

        // model transformation for arrowhead
        glm::mat4 translation = glm::translate(glm::vec3(sat2));
        glm::vec3 normal = glm::normalize(sat2 - sat1);
        glm::vec3 axis = glm::vec3(normal.z, 0.f, -normal.x);
        float angle = acos(normal.y);
        glm::mat4 rotation = glm::rotate(angle, axis);

        buffer_transformations.values.push_back(translation * rotation * scale);
    }

    info->number_vertices = scheduled_communications.vertexCount();
    buffer_lines.values.insert(
        buffer_lines.values.end(), scheduled_communications.vertices.begin(), scheduled_communications.vertices.end());

    // build satellite orientations
    info = getObjectInfo("orientation_lines");
    if (info == nullptr) {
        printf("Object info for '%s' was not created yet!.\n", "orientation_lines");
        assert(false);
        exit(EXIT_FAILURE);
    }
    info->offset_vertices = buffer_lines.size();
    Object orientation_lines;

    info_arrowhead = getObjectInfo("orientation_arrowhead");
    if (info_arrowhead != nullptr)
        info_arrowhead->base_instance = buffer_transformations.size(); // offset
    for (auto const& it : animation.satellite_orientations) {
        const Satellite& satellite = problem_instance.getSatellites().at(it.first);
        glm::vec3 position = satellite.cartesian_coordinates(sim_time) / real_world_scale;
        TimelineEvent<OrientationDetails> last_orientation = it.second.previousEvent(sim_time, false);
        TimelineEvent<OrientationDetails> next_orientation = it.second.prevailingEvent(sim_time, false);

        if (!last_orientation.isValid()) {
            last_orientation.t_begin = 0.f;
            last_orientation.data.orientation = glm::vec3(0.f);
        }

        if (!next_orientation.isValid()) {
            next_orientation.t_begin = 0.f;
            next_orientation.data.orientation = glm::vec3(0.f);
        }

        float angle =
            std::acos(glm::dot(last_orientation.data.orientation, next_orientation.data.orientation)); // [rad]
        float dt = sim_time - last_orientation.t_begin;

        glm::vec3 direction_vector = last_orientation.data.orientation;
        direction_vector = glm::rotate(direction_vector,
                                       std::min(angle, dt * satellite.getRotationSpeed()),
                                       glm::cross(direction_vector, next_orientation.data.orientation)) *
                           0.03f;

        // cones are used as antennas?
        if (problem_instance.getSatellites()[it.first].getConeAngle() <= 0.f) {
            // model transformation for arrowhead
            glm::vec3 rotation_axis = glm::vec3(direction_vector.z, 0.f, -direction_vector.x);
            float rotation_angle = acos(glm::normalize(direction_vector).y);
            glm::mat4 rotation = glm::rotate(rotation_angle, rotation_axis);
            glm::mat4 translation = glm::translate(glm::vec3(position + direction_vector));
            buffer_transformations.values.push_back(translation * scale * rotation);

            orientation_lines.add(OpenGLPrimitives::createLine(position, position + direction_vector, glm::vec4(1.0f)));
        }
    }

    auto info_cones = getObjectInfo("orientation_cones");
    if (info_cones != nullptr)
        info_cones->base_instance = buffer_transformations.size(); // offset
    for (auto const& it : animation.satellite_orientations) {
        const Satellite& satellite = problem_instance.getSatellites().at(it.first);
        glm::vec3 position = satellite.cartesian_coordinates(sim_time) / real_world_scale;
        TimelineEvent<OrientationDetails> last_orientation = it.second.previousEvent(sim_time, false);
        TimelineEvent<OrientationDetails> next_orientation = it.second.prevailingEvent(sim_time, false);

        if (!last_orientation.isValid()) {
            last_orientation.t_begin = 0.f;
            last_orientation.data.orientation = glm::vec3(0.f);
        }

        if (!next_orientation.isValid()) {
            next_orientation.t_begin = 0.f;
            next_orientation.data.orientation = glm::vec3(0.f);
        }

        float angle =
            std::acos(glm::dot(last_orientation.data.orientation, next_orientation.data.orientation)); // [rad]
        float dt = sim_time - last_orientation.t_begin;

        glm::vec3 direction_vector = last_orientation.data.orientation;
        // TODO what if the vectors are linearly dependent? Then the cross product will be 0!
        direction_vector = glm::rotate(direction_vector,
                                       std::min(angle, dt * satellite.getRotationSpeed()),
                                       glm::cross(direction_vector, next_orientation.data.orientation));

        // cones are used as antennas?
        if (problem_instance.getSatellites()[it.first].getConeAngle() > 0.f) {
            float length = next_orientation.data.cone_length;
            float cone_angle = problem_instance.getSatellites()[it.first].getConeAngle();
            float radius = length * tanf(cone_angle / 2.f);

            glm::mat4 size = glm::scale(glm::vec3(radius, length, radius));
            glm::mat4 translation_origin =
                glm::translate(glm::vec3(0.f, -length / 2.f, 0.f)); // move origin of cone to (0,0,0)
            glm::mat4 translation = glm::translate(glm::vec3(position));

            glm::vec3 rotation_axis = glm::vec3(-direction_vector.z, 0.f, direction_vector.x);
            float rotation_angle = acos(glm::normalize(-direction_vector).y);
            glm::mat4 rotation = glm::rotate(rotation_angle, rotation_axis);

            buffer_transformations.values.push_back(translation * rotation * translation_origin * size);
        }
    }

    info->number_vertices = orientation_lines.vertexCount();
    buffer_lines.values.insert(
        buffer_lines.values.end(), orientation_lines.vertices.begin(), orientation_lines.vertices.end());

    buffer_lines.pushToGPU();
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::prepareInstance(const PhysicalInstance& instance) {
    deleteInstance();
    state = INSTANCE;
    problem_instance = instance; // copy so visualization does not depend on original instance
    std::vector<Object> objects;

    // central mass
    Object sphere =
        OpenGLPrimitives::createSphere(problem_instance.getRadiusCentralMass() / real_world_scale, glm::vec3(0.0f), 35);
    sphere.name = "central_mass";
    sphere.gl_program = earth_prog;
    sphere.gl_vao = vao_satellites;
    objects.push_back(sphere);

    // orbits
    Object all_orbits;
    all_orbits.gl_draw_mode = GL_LINE_LOOP;
    all_orbits.name = "orbit";
    all_orbits.gl_program = basic_program;
    all_orbits.gl_vao = vao;
    for (const Satellite& o : problem_instance.getSatellites()) {
        // Orbit
        Object orbit = OpenGLPrimitives::createOrbit(o, real_world_scale, glm::vec3(0.0f));
        size_t offset = all_orbits.vertices.size(); // index 0 in orbit object has to refer the correct vertex
        all_orbits.vertices.insert(all_orbits.vertices.end(), orbit.vertices.begin(), orbit.vertices.end());
        all_orbits.elements.reserve(all_orbits.elements.capacity() + orbit.elements.size());
        all_orbits.elements.push_back(MAX_ELEMENT_ID); // restart GL_LINE_LOOP

        for (const auto& i : orbit.elements) {
            all_orbits.elements.push_back(i + static_cast<GLuint>(offset));
        }
    }
    objects.push_back(all_orbits);

    // Satellites
    Object satellites = OpenGLPrimitives::createSatellite();
    satellites.name = "satellites";
    satellites.gl_program = satellite_prog;
    satellites.gl_vao = vao_satellites;
    satellites.gl_element_type = GL_UNSIGNED_BYTE;
    satellites.drawInstanced = true;
    // for each satellite in instance we need one copy of the satellite object
    satellites.instance_count = problem_instance.getSatellites().size();
    objects.push_back(satellites);

    // Edges & orientations
    Object line_obj;
    line_obj.gl_draw_mode = GL_LINES;
    line_obj.name = "isl_network";
    line_obj.gl_program = basic_program;
    line_obj.gl_vao = vao_lines;
    objects.push_back(line_obj);
    line_obj.name = "scheduled_communications";
    objects.push_back(line_obj);
    line_obj.name = "orientation_lines";
    objects.push_back(line_obj);

    // build arrowheads for scheduled communications
    Object cone = OpenGLPrimitives::createCone(0.006f, 0.03f, glm::vec4(.55f, .1f, 1.f, 1.f));
    cone.name = "communications_arrowhead";
    cone.gl_program = satellite_prog;
    cone.gl_vao = vao_satellites;
    cone.gl_element_type = GL_UNSIGNED_BYTE;
    cone.drawInstanced = true;
    cone.instance_count = problem_instance.scheduled_communications.size();
    objects.push_back(cone);

    // build arrowheads for satellite orientation (if cones are used)
    Object orientation_cone = OpenGLPrimitives::createCone(1.0f, 1.0f, glm::vec4(1.f, 1.f, 1.f, .5f), 30U, true);
    orientation_cone.name = "orientation_cones";
    orientation_cone.gl_program = shaded_prog;
    orientation_cone.gl_vao = vao_satellites;
    orientation_cone.gl_element_type = GL_UNSIGNED_BYTE;
    orientation_cone.drawInstanced = true;
    orientation_cone.instance_count = 0;

    // build arrowheads for satellite orientations
    Object arrowhead_cone = OpenGLPrimitives::createCone(0.005f, 0.012f, glm::vec4(1.f));
    arrowhead_cone.name = "orientation_arrowhead";
    arrowhead_cone.gl_program = satellite_prog;
    arrowhead_cone.gl_vao = vao_satellites;
    arrowhead_cone.gl_element_type = GL_UNSIGNED_BYTE;
    arrowhead_cone.drawInstanced = true;
    arrowhead_cone.instance_count = 0;

    for (const auto& sat : problem_instance.getSatellites()) {
        if (sat.getConeAngle() > 0.f) {
            orientation_cone.instance_count++;
        } else {
            arrowhead_cone.instance_count++;
        }
    }

    objects.push_back(orientation_cone);
    objects.push_back(arrowhead_cone);

    // sort objects by their VAO/program in order to reduce sate changes
    pushStaticSceneToGPU(objects);
    std::sort(scene.begin(), scene.end());

    // build map to find objects by name
    for (int i = 0; i < scene.size(); i++) {
        const auto& obj = scene[i];
        if (obj.name != "") {
            auto res = object_names.insert({obj.name, i});
            if (!res.second) {
                printf("Insertion of object '%s' failed.\n", obj.name.c_str());
                assert(false);
                exit(EXIT_FAILURE);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::pushStaticSceneToGPU(const std::vector<Object>& scene_objects) {
    scene.clear();
    size_t vertex_size = 0;
    size_t element_size = 0;

    for (const auto& object : scene_objects) {
        vertex_size += object.totalVertexSize();
        element_size += object.totalElementSize();
    }

    // allocate memory on gpu
    glBindBuffer(GL_ARRAY_BUFFER, vbo_static);                              // set active
    glBufferData(GL_ARRAY_BUFFER, vertex_size, 0, GL_STATIC_DRAW);          // allocate memory
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_static);                      // set active
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, element_size, 0, GL_STATIC_DRAW); // allocate memory

    // push data to GPU
    size_t offset_vertices = 0;
    size_t offset_elements = 0;
    size_t vertex_count = 0;

    for (const auto& object : scene_objects) {
        ObjectInfo info(object);
        info.base_index = vertex_count;
        info.offset_elements = offset_elements;
        scene.push_back(info);

        size_t object_size = object.totalVertexSize(); // size of all vertices in byte

        if (object_size != 0) {
            // all static vertex data
            glBindBuffer(GL_ARRAY_BUFFER, vbo_static);
            glBufferSubData(GL_ARRAY_BUFFER, offset_vertices, object_size, &object.vertices[0]);
            offset_vertices += object_size;
        }

        // if object is defined by vertex id's, because multiple triangles uses the same vertices
        if (object.isElementObject()) {
            size_t object_element_size = object.totalElementSize(); // size of all vertex id's in byte
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_static);

            switch (object.gl_element_type) {
            case GL_UNSIGNED_SHORT:
                glBufferSubData(
                    GL_ELEMENT_ARRAY_BUFFER, offset_elements, object_element_size, &object.elements_16()[0]);
                break;
            case GL_UNSIGNED_BYTE:
                glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, offset_elements, object_element_size, &object.elements_8()[0]);
                break;
            default:
                glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, offset_elements, object_element_size, &object.elements[0]);
                break;
            }

            offset_elements += object_element_size;
        }
        vertex_count += object.vertexCount();
    }
}

// ------------------------------------------------------------------------------------------------

ObjectInfo* OpenGLWidget::getObjectInfo(const std::string& name) {
    auto result = object_names.find(name);
    if (result != object_names.end()) {
        if (result->second < scene.size())
            return &scene[result->second];
    }

    printf("There is no object with the name '%s'!\n", name.c_str());
    return nullptr;
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::buildGUI() {
    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Simulation control panel
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(340, 180), ImVec2(1500, 1500));
        ImGui::Begin("Simulation control panel"); // Create a window and append into it.

        ImGui::PushItemWidth(ImGui::GetFontSize() * -12);
        const char* btn_text = paused ? "Play" : "Pause";
        if (ImGui::Button(btn_text)) {
            paused = !paused;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            sim_time = 0.f;
            sim_speed = 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset camera")) {
            camera_rotation_angle = glm::vec2(0.f);
            zoom = 1.f;
        }

        ImGui::InputInt("Speed", &sim_speed);

        int t = (int)sim_time;
        ImGui::Text("t = %+id %ih %imin %isec", (t / 86400), (t / 3600) % 24, (t / 60) % 60, t % 60);

        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_None)) {
            static bool hide_satellites = false;
            if (ImGui::Checkbox("Hide satellites", &hide_satellites)) {
                auto info = getObjectInfo("satellites");
                if (info != nullptr)
                    info->enabled = !hide_satellites;
            }

            static bool hide_earth = false;
            if (ImGui::Checkbox("Hide earth", &hide_earth)) {
                auto info = getObjectInfo("central_mass");
                if (info != nullptr)
                    info->enabled = !hide_earth;
            }

            static bool hide_orbits = false;
            if (ImGui::Checkbox("Hide orbits", &hide_orbits)) {
                auto info = getObjectInfo("orbit");
                if (info != nullptr)
                    info->enabled = !hide_orbits;
            }

            static bool hide_isl = false;
            if (ImGui::Checkbox("Hide ISL-network", &hide_isl)) {
                auto info = getObjectInfo("isl_network");
                if (info != nullptr)
                    info->enabled = !hide_isl;
            }

            static bool hide_comms = false;
            if (ImGui::Checkbox("Hide scheduled communications", &hide_comms)) {
                auto info = getObjectInfo("scheduled_communications");
                if (info != nullptr)
                    info->enabled = !hide_comms;
                info = getObjectInfo("communications_arrowhead");
                if (info != nullptr)
                    info->enabled = !hide_comms;
            }

            static bool hide_orientations = false;
            if (ImGui::Checkbox("Hide satellite orientations", &hide_orientations)) {
                auto info = getObjectInfo("orientation_lines");
                if (info != nullptr)
                    info->enabled = !hide_orientations;
                info = getObjectInfo("orientation_arrowhead");
                if (info != nullptr)
                    info->enabled = !hide_orientations;
            }
        }

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / ImGui::GetIO().Framerate,
                    ImGui::GetIO().Framerate);
        ImGui::End();
    }
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::destroy() {
    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    deleteInstance();
    glDeleteProgram(basic_program);
    glDeleteProgram(satellite_prog);
    glDeleteProgram(earth_prog);
    glDeleteTextures(1, &texture_id[0]);
    glDeleteTextures(1, &texture_id[1]);
    glDeleteVertexArrays(1, &vao);
    glDeleteVertexArrays(1, &vao_lines);
    glDeleteBuffers(1, &ibo_static);
    glDeleteBuffers(1, &vbo_static);
    glDeleteBuffers(1, &vbo_uniforms);
    glfwDestroyWindow(window);
    glfwTerminate();
}

// ------------------------------------------------------------------------------------------------

void OpenGLWidget::deleteInstance() {
    state = EMPTY;
    scene.clear();
    animation = Animation();
    object_names.clear();
    sim_speed = 1;
    sim_time = 0.f;
}

} // namespace dmsc
