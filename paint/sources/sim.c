
#include "global.h"

bool sim_initialized = false;

void sim_init() {
	if (sim_initialized) {
		return;
	}
	physics_world_create();
	sim_initialized = true;
}

void sim_update() {
	render_path_raytrace_ready = false;

	if (sim_running) {
		trait_update();
		physics_world_update();
		iron_delay_idle_sleep();
	}
}

void sim_play() {
	sim_running = true;
}

void sim_stop() {
	sim_running = false;
	trait_stop();
}

void sim_add_body(object_t *o, physics_shape_t shape, f32 mass) {
	sim_init();
	physics_body_create(o, shape, mass);
}

typedef struct {
	i32 shape; // -1 = no physics
	f32 mass;
} sim_physics_t;

static any_imap_t *sim_physics_map = NULL; // object uid -> sim_physics_t

static sim_physics_t *sim_physics_find(object_t *o) {
	if (o == NULL || sim_physics_map == NULL) {
		return NULL;
	}
	return any_imap_get(sim_physics_map, o->uid);
}

static bool sim_physics_in_stage_of(stage_t *stage, object_t *o) {
	return stage == NULL || string_array_index_of(stage->objects, o->name) >= 0;
}

static bool sim_physics_in_stage(object_t *o) {
	return sim_physics_in_stage_of(tab_stages_get_stage(), o);
}

void sim_physics_clear() {
	sim_physics_map = NULL;
}

void sim_physics_store(object_t *o, i32 shape, f32 mass) {
	if (o == NULL) {
		return;
	}
	if (sim_physics_map == NULL) {
		sim_physics_map = any_imap_create();
	}
	sim_physics_t *p = any_imap_get(sim_physics_map, o->uid);
	if (p == NULL) {
		p = ALLOC_INIT(sim_physics_t, {0});
		any_imap_set(sim_physics_map, o->uid, p);
	}
	p->shape = shape;
	p->mass  = mass;
}

void sim_physics_set(object_t *o, i32 shape, f32 mass) {
	if (o == NULL) {
		return;
	}
	sim_physics_store(o, shape, mass);
	if (o->_->body != NULL) {
		physics_body_remove(o->_->body);
	}
	if (shape >= 0 && sim_physics_in_stage(o)) {
		sim_add_body(o, (physics_shape_t)shape, mass);
	}
}

void sim_physics_set_mass(object_t *o, f32 mass) {
	if (o == NULL) {
		return;
	}
	sim_physics_store(o, sim_physics_get_shape(o), mass);
	if (o->_->body != NULL) {
		physics_body_set_mass(o->_->body, mass); // Zero mass = static
	}
}

i32 sim_physics_get_shape(object_t *o) {
	if (o != NULL && o->_->body != NULL) {
		return o->_->body->shape;
	}
	sim_physics_t *p = sim_physics_find(o);
	return p != NULL ? p->shape : -1;
}

f32 sim_physics_get_mass(object_t *o) {
	if (o != NULL && o->_->body != NULL) {
		return o->_->body->mass;
	}
	sim_physics_t *p = sim_physics_find(o);
	return p != NULL ? p->mass : 0.0;
}

static bool sim_physics_is_shared(i32 shape) {
	return shape == PHYSICS_SHAPE_MESH || shape == PHYSICS_SHAPE_TERRAIN;
}

void sim_physics_apply_stage(stage_t *stage) {
	bool shared_cleared = false;

	// Drop the bodies which are no longer in the stage
	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		object_t      *o     = g_project->_->paint_objects->buffer[i]->base;
		sim_physics_t *p     = sim_physics_find(o);
		i32            shape = p != NULL ? p->shape : -1;
		if (o->_->body == NULL || (shape >= 0 && sim_physics_in_stage_of(stage, o))) {
			continue;
		}
		shared_cleared = shared_cleared || sim_physics_is_shared(o->_->body->shape);
		physics_body_remove(o->_->body);
	}

	for (i32 i = 0; i < g_project->_->paint_objects->length; ++i) {
		object_t      *o     = g_project->_->paint_objects->buffer[i]->base;
		sim_physics_t *p     = sim_physics_find(o);
		i32            shape = p != NULL ? p->shape : -1;
		if (shape < 0 || !sim_physics_in_stage_of(stage, o)) {
			continue;
		}
		if (o->_->body != NULL) {
			if (!shared_cleared || !sim_physics_is_shared(shape)) {
				continue;
			}
			physics_body_remove(o->_->body);
		}
		sim_add_body(o, (physics_shape_t)shape, p->mass);
	}
}

static void sim_shift_object_masks(i32 from) {
	if (g_project->_->layers != NULL) {
		for (i32 i = 0; i < g_project->_->layers->length; ++i) {
			slot_layer_t *l = g_project->_->layers->buffer[i];
			if (l->object_mask >= from) {
				++l->object_mask;
			}
		}
	}
	if (g_context->layer_filter >= from) {
		++g_context->layer_filter;
	}
}

mesh_object_t *sim_duplicate_object(mesh_object_t *so) {
	// Mesh
	if (so == NULL) {
		return NULL;
	}

	mesh_data_t   *data = so->data;
	mesh_object_t *dup  = scene_add_mesh_object(data, so->material, so->base->parent);
	transform_set_matrix(dup->base->transform, so->base->transform->local);

	// Insert below the original
	i32 index = array_index_of(g_project->_->paint_objects, so);
	i32 at    = index < 0 ? g_project->_->paint_objects->length : index + 1;
	array_insert((any_array_t *)g_project->_->paint_objects, at, dup);
	sim_shift_object_masks(at + 1);

	// Ensure unique name
	dup->base->name = string_copy(_import_mesh_unique_name(so->base->name));
	tab_stages_add_object(dup->base->name);

	// Material override
	i32 mat_index = tab_meshes_get_override(so);
	if (mat_index >= 0) {
		tab_meshes_set_override_data(dup, mat_index, so->material);
		g_project->mesh_materials = i32_array_create(0);
	}

	// Physics
	i32 shape = sim_physics_get_shape(so->base);
	if (shape >= 0) {
		sim_physics_set(dup->base, shape, sim_physics_get_mass(so->base));
	}

	tab_meshes_sort_hierarchy();
	tab_timeline_sync();

	return dup;
}

void sim_duplicate() {
	mesh_object_t *dup = sim_duplicate_object(g_context->paint_object);
	if (dup != NULL) {
		g_context->paint_object                           = dup;
		ui_header_handle->redraws                         = 2;
		ui_base_hwnds->buffer[TAB_AREA_SIDEBAR0]->redraws = 2;
	}
	util_mesh_merge(NULL);
	g_context->ddirty = 2;
}

void sim_delete() {
	if (g_project->_->paint_objects->length < 2) {
		return;
	}
	tab_meshes_draw_context_menu_delete(g_context->paint_object);
}
