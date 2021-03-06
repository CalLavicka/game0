#include "Game.hpp"

#include "gl_errors.hpp" //helper for dumpping OpenGL error messages
#include "read_chunk.hpp" //helper for reading a vector of structures from a file
#include "data_path.hpp" //helper to get paths relative to executable

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/random.hpp>

#include <iostream>
#include <fstream>
#include <map>
#include <cstddef>
#include <random>
#include <cmath>

#define PI 3.141592f

#define NUM_TARGETS 10

// Some helpful functions for vectors
float mag(glm::vec2 vec) {
	return glm::sqrt(vec.x * vec.x + vec.y * vec.y);
}

void normalize(glm::vec2 &vec) {
	vec /= mag(vec);
}

Game::Target Game::create_target(bool golden) {
	Target target = Target();
	target.mesh = golden ? golden_egg_mesh : target_mesh;
	target.points = 10;
	target.golden = golden;
	target.position.y = glm::linearRand(1.0f, 9.0f);
	target.position.x = glm::linearRand(-4.5f, 4.5f);

	//std::cout << "Position" << target.position.x << target.position.y << std::endl;

	return target;
}

//helper defined later; throws if shader compilation fails:
static GLuint compile_shader(GLenum type, std::string const &source);

Game::Game() {
	{ //create an opengl program to perform sun/sky (well, directional+hemispherical) lighting:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 object_to_clip;\n"
			"uniform mat4x3 object_to_light;\n"
			"uniform mat3 normal_to_light;\n"
			"layout(location=0) in vec4 Position;\n" //note: layout keyword used to make sure that the location-0 attribute is always bound to something
			"in vec3 Normal;\n"
			"in vec4 Color;\n"
			"out vec3 position;\n"
			"out vec3 normal;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"	gl_Position = object_to_clip * Position;\n"
			"	position = object_to_light * Position;\n"
			"	normal = normal_to_light * Normal;\n"
			"	color = Color;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 sun_direction;\n"
			"uniform vec3 sun_color;\n"
			"uniform vec3 sky_direction;\n"
			"uniform vec3 sky_color;\n"
			"in vec3 position;\n"
			"in vec3 normal;\n"
			"in vec4 color;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	vec3 total_light = vec3(0.0, 0.0, 0.0);\n"
			"	vec3 n = normalize(normal);\n"
			"	{ //sky (hemisphere) light:\n"
			"		vec3 l = sky_direction;\n"
			"		float nl = 0.5 + 0.5 * dot(n,l);\n"
			"		total_light += nl * sky_color;\n"
			"	}\n"
			"	{ //sun (directional) light:\n"
			"		vec3 l = sun_direction;\n"
			"		float nl = max(0.0, dot(n,l));\n"
			"		total_light += nl * sun_color;\n"
			"	}\n"
			"	fragColor = vec4(color.rgb * total_light, color.a);\n"
			"}\n"
		);

		simple_shading.program = glCreateProgram();
		glAttachShader(simple_shading.program, vertex_shader);
		glAttachShader(simple_shading.program, fragment_shader);
		//shaders are reference counted so this makes sure they are freed after program is deleted:
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//link the shader program and throw errors if linking fails:
		glLinkProgram(simple_shading.program);
		GLint link_status = GL_FALSE;
		glGetProgramiv(simple_shading.program, GL_LINK_STATUS, &link_status);
		if (link_status != GL_TRUE) {
			std::cerr << "Failed to link shader program." << std::endl;
			GLint info_log_length = 0;
			glGetProgramiv(simple_shading.program, GL_INFO_LOG_LENGTH, &info_log_length);
			std::vector< GLchar > info_log(info_log_length, 0);
			GLsizei length = 0;
			glGetProgramInfoLog(simple_shading.program, GLsizei(info_log.size()), &length, &info_log[0]);
			std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
			throw std::runtime_error("failed to link program");
		}
	}

	{ //read back uniform and attribute locations from the shader program:
		simple_shading.object_to_clip_mat4 = glGetUniformLocation(simple_shading.program, "object_to_clip");
		simple_shading.object_to_light_mat4x3 = glGetUniformLocation(simple_shading.program, "object_to_light");
		simple_shading.normal_to_light_mat3 = glGetUniformLocation(simple_shading.program, "normal_to_light");

		simple_shading.sun_direction_vec3 = glGetUniformLocation(simple_shading.program, "sun_direction");
		simple_shading.sun_color_vec3 = glGetUniformLocation(simple_shading.program, "sun_color");
		simple_shading.sky_direction_vec3 = glGetUniformLocation(simple_shading.program, "sky_direction");
		simple_shading.sky_color_vec3 = glGetUniformLocation(simple_shading.program, "sky_color");

		simple_shading.Position_vec4 = glGetAttribLocation(simple_shading.program, "Position");
		simple_shading.Normal_vec3 = glGetAttribLocation(simple_shading.program, "Normal");
		simple_shading.Color_vec4 = glGetAttribLocation(simple_shading.program, "Color");
	}

	struct Vertex {
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 28, "Vertex should be packed.");

	{ //load mesh data from a binary blob:
		std::ifstream blob(data_path("meshes.blob"), std::ios::binary);
		//The blob will be made up of three chunks:
		// the first chunk will be vertex data (interleaved position/normal/color)
		// the second chunk will be characters
		// the third chunk will be an index, mapping a name (range of characters) to a mesh (range of vertex data)

		//read vertex data:
		std::vector< Vertex > vertices;
		read_chunk(blob, "dat0", &vertices);

		//read character data (for names):
		std::vector< char > names;
		read_chunk(blob, "str0", &names);

		//read index:
		struct IndexEntry {
			uint32_t name_begin;
			uint32_t name_end;
			uint32_t vertex_begin;
			uint32_t vertex_end;
		};
		static_assert(sizeof(IndexEntry) == 16, "IndexEntry should be packed.");

		std::vector< IndexEntry > index_entries;
		read_chunk(blob, "idx0", &index_entries);

		if (blob.peek() != EOF) {
			std::cerr << "WARNING: trailing data in meshes file." << std::endl;
		}

		//upload vertex data to the graphics card:
		glGenBuffers(1, &meshes_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		//create map to store index entries:
		std::map< std::string, Mesh > index;
		for (IndexEntry const &e : index_entries) {
			if (e.name_begin > e.name_end || e.name_end > names.size()) {
				throw std::runtime_error("invalid name indices in index.");
			}
			if (e.vertex_begin > e.vertex_end || e.vertex_end > vertices.size()) {
				throw std::runtime_error("invalid vertex indices in index.");
			}
			Mesh mesh;
			mesh.first = e.vertex_begin;
			mesh.count = e.vertex_end - e.vertex_begin;
			auto ret = index.insert(std::make_pair(
				std::string(names.begin() + e.name_begin, names.begin() + e.name_end),
				mesh));
			if (!ret.second) {
				throw std::runtime_error("duplicate name in index.");
			}
		}

		//look up into index map to extract meshes:
		auto lookup = [&index](std::string const &name) -> Mesh {
			auto f = index.find(name);
			if (f == index.end()) {
				throw std::runtime_error("Mesh named '" + name + "' does not appear in index.");
			}
			return f->second;
		};
		//tile_mesh = lookup("Tile");
		//cursor_mesh = lookup("Cursor");
		player_mesh = lookup("Doll");
		target_mesh = lookup("Egg.001");
		enemy_mesh = lookup("Cube");
		cursor_mesh = lookup("Aim");
		golden_egg_mesh = lookup("Egg");
	}

	{ //create vertex array object to hold the map from the mesh vertex buffer to shader program attributes:
		glGenVertexArrays(1, &meshes_for_simple_shading_vao);
		glBindVertexArray(meshes_for_simple_shading_vao);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		//note that I'm specifying a 3-vector for a 4-vector attribute here, and this is okay to do:
		glVertexAttribPointer(simple_shading.Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
		glEnableVertexAttribArray(simple_shading.Position_vec4);
		if (simple_shading.Normal_vec3 != -1U) {
			glVertexAttribPointer(simple_shading.Normal_vec3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Normal));
			glEnableVertexAttribArray(simple_shading.Normal_vec3);
		}
		if (simple_shading.Color_vec4 != -1U) {
			glVertexAttribPointer(simple_shading.Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
			glEnableVertexAttribArray(simple_shading.Color_vec4);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GL_ERRORS();

	//----------------
	//set up game board with meshes and rolls:
	//board_meshes.reserve(board_size.x * board_size.y);
	//board_rotations.reserve(board_size.x * board_size.y);
	std::mt19937 mt(0xbead1234);

	reset_game();
}

void Game::reset_game() {

	score = 0;
	golden_score = 250;
	game_state = aiming;
	angle = 90.0f;
	power = 0.0f;

	golden_active = false;
	golden_time = 0.0f;

	player = Player();
	player.mesh = player_mesh;
	player.position = glm::vec2(0.0f, 0.0f);
	player.velocity = glm::vec2(0.0f, 0.0f);

	enemies.clear();
	Enemy enemy = Enemy();
	enemy.mesh = enemy_mesh;
	enemy.position = glm::vec2(3.0f, 3.0f);
	enemy.speed = 1.0f;
	enemy.direction = glm::linearRand(0.0f, 360.0f);
	enemies.push_back(enemy);
	enemies_spawned = 1;

	eggs = 0;
	golden_eggs = 0;

	targets.clear();
	for (int i=0; i<NUM_TARGETS; i++) {
		Target target = create_target(false);
		targets.push_back(target);
	}
}

Game::~Game() {
	glDeleteVertexArrays(1, &meshes_for_simple_shading_vao);
	meshes_for_simple_shading_vao = -1U;

	glDeleteBuffers(1, &meshes_vbo);
	meshes_vbo = -1U;

	glDeleteProgram(simple_shading.program);
	simple_shading.program = -1U;

	GL_ERRORS();
}

bool Game::handle_event(SDL_Event const &evt, glm::uvec2 window_size) {
	//ignore any keys that are the result of automatic key repeat:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat) {
		return false;
	}

	if (evt.type == SDL_KEYDOWN || evt.type == SDL_KEYUP) {
		if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
			controls.angle_left = (evt.type == SDL_KEYDOWN);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
			controls.angle_right = (evt.type == SDL_KEYDOWN);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_SPACE && evt.type == SDL_KEYDOWN && game_state == aiming) {
			controls.power_up = true;
			game_state = charging;
			return true;
		}
	}

	if (game_state == charging && evt.key.keysym.scancode == SDL_SCANCODE_SPACE && evt.type == SDL_KEYUP) {
		controls.power_up = false;
		game_state = flying;
		
		float velx = glm::cos(angle * PI / 180.0f) * power;
		float vely = glm::sin(angle * PI / 180.0f) * power;
		player.velocity = glm::vec2(velx, vely);
		return true;
	}
	return false;
}

bool collision(glm::vec2 p1, glm::vec2 p2, float dist) {
	auto dif = p1 - p2;
	return (dif.x * dif.x + dif.y * dif.y) <= dist * dist;
}

void Game::update(float elapsed) {
	switch(game_state) {
	case charging:
		// Add to power
		power = glm::min(power + 10.0f * elapsed, 12.0f);
	case aiming:
		// Update aiming
		if (controls.angle_left) {
			angle = glm::min(angle + 50.0f * elapsed, 160.0f);
		}
		if (controls.angle_right) {
			angle = glm::max(angle - 50.0f * elapsed, 20.0f);
		}

		// Update golden
		golden_active = golden_time > 0.0f;
		break;
	case flying:
		// Update player
		player.position += player.velocity * elapsed;
		player.velocity.y -= elapsed * 6.0f;
		if (player.position.y <= 0) {
			player.position.y = 0;
			game_state = aiming;
			angle = 90;
			power = 0;
			player.velocity = glm::vec2(0.0f, 0.0f);

			while (targets.size() < NUM_TARGETS) {
				Target target;
				if (score > golden_score) {
					target = create_target(true);
					golden_score += 290;
				} else {
					target = create_target(false);
				}
				targets.push_back(target);
			}

			if (score > enemies_spawned * 100) {
				Enemy enemy = Enemy();
				enemy.mesh = enemy_mesh;
				if (enemies.size() > 0) {
					enemy.position = enemies[enemies.size() - 1].position;
				} else {
					enemy.position = glm::vec2(-5.0f, 10.0f);
				}
				enemy.speed = 1.0f + enemies_spawned * 0.05f;
				enemy.direction = glm::linearRand(0.0f, 360.0f);
				enemies.push_back(enemy);
				enemies_spawned++;
			}
		}
		if (player.position.x >= 5.0f) {
			player.velocity.x = -glm::abs(player.velocity.x);
			player.position.x = 5.0f - (player.position.x - 5.0f);
		} else if (player.position.x <= -5.0f) {
			player.velocity.x = glm::abs(player.velocity.x);
			player.position.x = -5.0f - (player.position.x + 5.0f);
		}
	default:
		break;
	}

	golden_time = glm::max(0.0f, golden_time - elapsed);

	// Check targets
	for (size_t i = 0; i < targets.size(); i++) {
		Target target = targets[i];
		// Check for a collision
		if (collision(target.position, player.position, target.radius + player.radius)) {
			// COLLISION
			score += target.points;
			targets.erase(targets.begin() + i);
			i--;

			if (target.golden) {
				golden_active = true;
				golden_time += 7.5f;
				golden_eggs++;
			} else {
				eggs++;
			}
		}
	}

	// Update enemies
	for (size_t i = 0; i < enemies.size(); i++) {
		glm::vec2 dir;
		float angle;

		Enemy &enemy = enemies[i];
		if (golden_active) {
			// Start fleeing, but switch states once golden runs out
			enemy.state = flee;
			enemy.target_time = 0.0f;
		}

		// Update enemy based on position
		angle = enemy.direction * PI / 180.0f;
		dir = glm::vec2(glm::cos(angle) * enemy.speed * elapsed, glm::sin(angle) * enemy.speed * elapsed);
		enemy.position += dir;

		switch(enemy.state) {
		case chase:
			// Update direction to player
			dir = player.position - enemy.position;
			angle = atan2(dir.y, dir.x) * 180.0f / PI;
			angle = fmod(enemy.direction - angle, 360.0f);
			if (angle < 0.0f) {
				angle += 360.0f;
			}
			if (angle < 180.0f) {
				enemy.direction += glm::linearRand(-80.0f, -60.0f) * elapsed;
			} else {
				enemy.direction += glm::linearRand(60.0f, 80.0f) * elapsed;
			}

			break;
		case flee:
			// Update direction away from player
			dir = enemy.position - player.position;
			angle = atan2(dir.y, dir.x) * 180.0f / PI;
			angle = fmod(enemy.direction - angle, 360.0f);
			if (angle < 0.0f) {
				angle += 360.0f;
			}
			if (angle < 180.0f) {
				enemy.direction += glm::linearRand(-80.0f, -60.0f) * elapsed;
			} else {
				enemy.direction += glm::linearRand(60.0f, 80.0f) * elapsed;
			}
			break;
		case patrol:
			// Swap direction periodically
			enemy.time_traveled += elapsed;
			if (enemy.time_traveled >= 3.0f) {
				enemy.time_traveled = 0.0f;
				enemy.direction = 180.0f + enemy.direction;
			}
			break;
		case wander:
			// Pick direction somewhat randomly, weighted towards center
			dir = glm::vec2(0.0f, 5.0f) - enemy.position;
			angle = atan2(dir.y, dir.x) * 180.0f / PI;

			// Get difference 
			angle = fmod(enemy.direction - angle, 360.0f);
			if (angle < 0.0f) {
				angle += 360.0f;
			}
			if (angle < 180.0f) {
				enemy.direction += glm::linearRand(-60.0f, 20.0f) * elapsed;
			} else {
				enemy.direction += glm::linearRand(-20.0f, 60.0f) * elapsed;
			}
			break;
		case circle:
			// Go in circle
			enemy.direction += 60.0f * elapsed;
			break;
		case hunt:
			// Grab target ahead of player
			auto target = player.position + player.velocity * 1.0f;
			
			dir = target - enemy.position;
			angle = atan2(dir.y, dir.x) * 180.0f / PI;
			angle = fmod(enemy.direction - angle, 360.0f);
			if (angle < 0.0f) {
				angle += 360.0f;
			}
			if (angle < 180.0f) {
				enemy.direction += glm::linearRand(-80.0f, -60.0f) * elapsed;
			} else {
				enemy.direction += glm::linearRand(60.0f, 80.0f) * elapsed;
			}
			break;
		}

		// Make sure within bounds
		enemy.position.x = glm::min(glm::max(enemy.position.x, -4.8f), 4.8f);
		enemy.position.y = glm::min(glm::max(enemy.position.y, 0.3f), 9.5f);

		// Check for a collision
		if (collision(enemy.position, player.position, enemy.radius + player.radius + (golden_active ? 0.5f : 0.0f))) {
			if (golden_active) {
				enemies.erase(enemies.begin() + i);
				i--;
				continue;
			} else {
				reset_game();
				return;
			}
		}

		// Change AI (only when player is grounded)
		enemy.state_time += elapsed;
		if (enemy.state_time > enemy.target_time && game_state != flying && !golden_active) {
			int state_roll = glm::linearRand(0, 10);
			if (state_roll <= 2) {
				enemy.state = chase;
			} else if (state_roll == 3) {
				enemy.state = flee;
			} else if (state_roll <= 6) {
				enemy.state = patrol;
			} else if (state_roll <= 8) {
				enemy.state = wander;
			} else if (state_roll == 9) {
				enemy.state = circle;
			} else {
				enemy.state = hunt;
			}
			enemy.state_time = 0.0f;
			enemy.target_time = glm::linearRand(7.0f, 20.0f);

			// Some initialization
			switch(enemy.state) {
			case patrol:
				enemy.time_traveled = 0.0f;
			case circle:
			case wander:
				enemy.direction = glm::linearRand(0.0f, 360.0f);
				break;
			default:
				break;
			}
		}
	}
}

// Some helpers for common matrices
glm::mat4 rot_mat(float const angle) {
	float sintheta = glm::sin(angle * PI / 180.0f);
	float costheta = glm::cos(angle * PI / 180.0f);
	return glm::mat4(
		sintheta, costheta, 0.0f, 0.0f,
		costheta, -sintheta, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	);
}

glm::mat4 scale_mat(float const scalex, float const scaley) {
	return glm::mat4(
		scalex, 0.0f, 0.0f, 0.0f,
		0.0f, scaley, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	);
}

glm::mat4 trans_mat(float const transx, float const transy, float const transz) {
	return glm::mat4(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		transx, transy, transz, 1.0f
	);
}

glm::mat4 face1 = glm::mat4(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, glm::cos(PI / 2.0f), -glm::sin(PI / 2.0f), 0.0f,
	0.0f, glm::sin(PI / 2.0f), glm::cos(PI / 2.0f), 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
);

glm::mat4 face2 = glm::mat4(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, glm::cos(-PI / 2.0f), -glm::sin(-PI / 2.0f), 0.0f,
	0.0f, glm::sin(-PI / 2.0f), glm::cos(-PI / 2.0f), 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
);

glm::mat4 face3 = glm::mat4(
	glm::cos(-PI / 2.0f), 0.0f, glm::sin(-PI / 2.0f), 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	-glm::sin(-PI / 2.0f), 0.0f, glm::cos(-PI / 2.0f), 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
);

glm::mat4 face4 = glm::mat4(
	glm::cos(PI / 2.0f), 0.0f, glm::sin(PI / 2.0f), 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	-glm::sin(PI / 2.0f), 0.0f, glm::cos(PI / 2.0f), 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
);

void Game::draw(glm::uvec2 drawable_size) {
	float aspect = float(drawable_size.x) / float(drawable_size.y);

	//Set up a transformation matrix to fit the board in the window:
	glm::mat4 world_to_clip;
	{

		//want scale such that board * scale fits in [-aspect,aspect]x[-1.0,1.0] screen box:
		float scale = 0.2f;/*glm::min(
			2.0f * aspect / float(board_size.x),
			2.0f / float(board_size.y)
		);*/

		// Make sure 10x10 box is always onscreen
		if (aspect < 1.0f) {
			scale = scale * aspect;
		}

		//center of board will be placed at center of screen:
		float centerY = 5.0f;
		if (aspect < 1.0f) {
			centerY = 10.0f - (5.0f / aspect);
		}
		glm::vec2 center = glm::vec2(0.0f, centerY);//0.5f * glm::vec2(board_size);

		//NOTE: glm matrices are specified in column-major order
		world_to_clip = glm::mat4(
			scale / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f,-1.0f, 0.0f,
			-(scale / aspect) * center.x, -scale * center.y, 0.0f, 1.0f
		);
	}

	//set up graphics pipeline to use data from the meshes and the simple shading program:
	glBindVertexArray(meshes_for_simple_shading_vao);
	glUseProgram(simple_shading.program);

	glUniform3fv(simple_shading.sun_color_vec3, 1, glm::value_ptr(glm::vec3(0.81f, 0.81f, 0.76f)));
	glUniform3fv(simple_shading.sun_direction_vec3, 1, glm::value_ptr(glm::normalize(glm::vec3(-0.2f, 0.2f, 1.0f))));
	glUniform3fv(simple_shading.sky_color_vec3, 1, glm::value_ptr(glm::vec3(0.2f, 0.2f, 0.3f)));
	glUniform3fv(simple_shading.sky_direction_vec3, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));

	//helper function to draw a given mesh with a given transformation:
	auto draw_mesh = [&](Mesh const &mesh, glm::mat4 const &object_to_world) {
		//set up the matrix uniforms:
		if (simple_shading.object_to_clip_mat4 != -1U) {
			glm::mat4 object_to_clip = world_to_clip * object_to_world;
			glUniformMatrix4fv(simple_shading.object_to_clip_mat4, 1, GL_FALSE, glm::value_ptr(object_to_clip));
		}
		if (simple_shading.object_to_light_mat4x3 != -1U) {
			glUniformMatrix4x3fv(simple_shading.object_to_light_mat4x3, 1, GL_FALSE, glm::value_ptr(object_to_world));
		}
		if (simple_shading.normal_to_light_mat3 != -1U) {
			//NOTE: if there isn't any non-uniform scaling in the object_to_world matrix, then the inverse transpose is the matrix itself, and computing it wastes some CPU time:
			glm::mat3 normal_to_world = glm::inverse(glm::transpose(glm::mat3(object_to_world)));
			glUniformMatrix3fv(simple_shading.normal_to_light_mat3, 1, GL_FALSE, glm::value_ptr(normal_to_world));
		}

		//draw the mesh:
		glDrawArrays(GL_TRIANGLES, mesh.first, mesh.count);
	};

	draw_mesh(player.mesh, trans_mat(player.position.x, player.position.y, -0.5f));

	// Draw enemies
	for (Enemy enemy : enemies) {
		auto mat = trans_mat(enemy.position.x, enemy.position.y, -0.5f);
		if (golden_active) {
			mat = mat * face2;
		} else {
			switch(enemy.state) {
				case chase:
				case hunt:
					mat = mat * face1;
					break;
				case patrol:
				case circle:
					mat = mat * face4;
					break;
				case wander:
				case flee:
					mat = mat * face3;
					break;
			}
		}
		draw_mesh(enemy.mesh, mat);
	}

	// Draw targets
	for (Target target : targets) {

		//std::cout << "Drawing" << target.position.x << target.position.y << std::endl;
		draw_mesh(target.mesh, trans_mat(target.position.x, target.position.y, -0.7f) * scale_mat(2.0f, 2.0f));
	}

	glm::mat4 aimmat = rot_mat(180.0f - angle);

	if (game_state == aiming) {
		draw_mesh(cursor_mesh, trans_mat(player.position.x, player.position.y, -1.5f) * aimmat * scale_mat(0.1f, 2.3f) * trans_mat(0.0f, -1.0f, 0.0f));
	}

	if (game_state == charging) {
		draw_mesh(cursor_mesh, trans_mat(player.position.x, player.position.y, -1.5f) * aimmat * scale_mat(0.1f, power / 6.0f) * trans_mat(0.0f, -1.0f, 0.0f) * face1);
	}

	
	{ // Draw eggs gathered
		uint32_t egg5 = eggs / 5;
		uint32_t egg1 = eggs % 5;
		uint32_t gegg5 = golden_eggs / 5;
		uint32_t gegg1 = golden_eggs % 5;

		float minX;
		float maxX;
		float ypos;

		if (aspect > 1.0f) {
			float extra = (aspect - 1.0f) * 5.0f;
			if (extra < 1.0f) {
				// Can't really render on screen, render off screen
				minX = -1000.0f;
			} else {
				minX = -(4.6f + extra);
			}
			maxX = -5.3f;
			ypos = 9.0f;
		} else {
			minX = -4.6f;
			maxX = 5.0f;
			ypos = -1.2f;
		}
		float xpos = minX;

		while(egg5 > 0) {

			if (xpos >= maxX - 0.4f) {
				xpos = minX;
				ypos -= 1.0f;
			}

			// Draw egg
			draw_mesh(target_mesh, trans_mat(xpos, ypos, -1.0f) * scale_mat(1.5f, 1.5f));

			xpos += 0.8f;
			egg5--;
		}

		while(egg1 > 0) {
			if (xpos >= maxX - 0.25f) {
				xpos = minX;
				ypos -= 1.0f;
			}

			// Draw egg
			draw_mesh(target_mesh, trans_mat(xpos, ypos, -1.0f) * scale_mat(0.75f, 0.75f));

			xpos += 0.5f;
			egg1--;
		}

		while(gegg5 > 0) {

			if (xpos >= maxX - 0.4f) {
				xpos = minX;
				ypos -= 1.0f;
			}

			// Draw egg
			draw_mesh(golden_egg_mesh, trans_mat(xpos, ypos, -1.0f) * scale_mat(1.5f, 1.5f));

			xpos += 0.8f;
			gegg5--;
		}

		while(gegg1 > 0) {
			if (xpos >= maxX - 0.25f) {
				xpos = minX;
				ypos -= 1.0f;
			}

			// Draw egg
			draw_mesh(golden_egg_mesh, trans_mat(xpos, ypos, -1.0f) * scale_mat(0.75f, 0.75f));

			xpos += 0.5f;
			gegg1--;
		}
	}
	

	// Draw walls
	draw_mesh(enemy_mesh, trans_mat(-5.3f, 5.0f, 0.0f) * scale_mat(1.0f, 100.0f));
	draw_mesh(enemy_mesh, trans_mat(5.3f, 5.0f, 0.0f) * scale_mat(1.0f, 100.0f));

	// Draw floor
	draw_mesh(enemy_mesh, trans_mat(0.0f, -0.3f, 0.0f) * scale_mat(100.0f, 1.0f));

	glUseProgram(0);

	GL_ERRORS();
}



//create and return an OpenGL vertex shader from source:
static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, GLsizei(info_log.size()), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}
