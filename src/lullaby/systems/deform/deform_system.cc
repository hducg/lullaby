/*
Copyright 2017 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS-IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "lullaby/systems/deform/deform_system.h"

#include "lullaby/base/dispatcher.h"
#include "lullaby/systems/render/render_system.h"
#include "lullaby/systems/transform/transform_system.h"
#include "lullaby/util/mathfu_fb_conversions.h"
#include "lullaby/util/mesh_util.h"

namespace lull {
namespace {
const HashValue kDeformerHash = Hash("DeformerDef");
const HashValue kWaypointDeformerHash = Hash("WaypointDeformerDef");
const HashValue kDeformedHash = Hash("DeformedDef");

// Returns the distance of the coordinate transform from the Y-axis.
float GetRadius(const mathfu::mat4& mat) {
  return std::sqrt(mat(0, 3) * mat(0, 3) + mat(2, 3) * mat(2, 3));
}

// Returns the standard transformation matrix given the SQT and a nullable
// world_from_parent_mat
mathfu::mat4 CalculateTransformMatrixFromParent(
    const Sqt& sqt, const mathfu::mat4* world_from_parent_mat) {
  const auto parent_from_local_mat = CalculateTransformMatrix(sqt);
  return world_from_parent_mat
             ? (*world_from_parent_mat) * parent_from_local_mat
             : parent_from_local_mat;
}

// Calculates the parameterization axis for a path by finding the unit vector
// pointing to the last point in the path from the first point. Returns an empty
// optional if their are less than 2 waypoints.
Optional<mathfu::vec3> CalculateParameterizationAxis(const WaypointPath& path) {
  if (path.waypoints() == nullptr || path.waypoints()->size() < 1) {
    return Optional<mathfu::vec3>();
  }

  auto end_fbb = --path.waypoints()->end();
  mathfu::vec3 end;
  MathfuVec3FromFbVec3(end_fbb->original_position(), &end);
  auto begin_fbb = path.waypoints()->begin();
  mathfu::vec3 begin;
  MathfuVec3FromFbVec3(begin_fbb->original_position(), &begin);
  return (end - begin).Normalized();
}

}  // namespace

DeformSystem::DeformSystem(Registry* registry)
    : System(registry), deformers_(16), deformed_(16) {
  RegisterDef(this, kDeformerHash);
  RegisterDef(this, kDeformedHash);
  RegisterDependency<RenderSystem>(this);
  RegisterDependency<TransformSystem>(this);

  Dispatcher* dispatcher = registry_->Get<Dispatcher>();
  dispatcher->Connect(this, [this](const ParentChangedEvent& event) {
    OnParentChanged(event);
  });
}

DeformSystem::~DeformSystem() {
  auto* dispatcher = registry_->Get<Dispatcher>();
  if (dispatcher) {
    dispatcher->DisconnectAll(this);
  }
}

void DeformSystem::Create(Entity e, HashValue type, const Def* def) {
  if (type == kDeformerHash) {
    const DeformerDef* deformer_def = ConvertDef<DeformerDef>(def);
    if (deformer_def->deform_mode() == DeformMode_Waypoint &&
        (deformer_def->waypoint_paths() == nullptr ||
         deformer_def->waypoint_paths()->size() < 1)) {
      LOG(DFATAL) << "Waypoint deformations must have at least one path";
      return;
    }
    Deformed* deformed = deformed_.Emplace(e);
    Deformer* deformer = deformers_.Emplace(e);
    deformer->radius = deformer_def->horizontal_radius();
    deformer->mode = deformer_def->deform_mode();
    deformer->clamp_angle = deformer_def->clamp_angle();

    if (deformer_def->deform_mode() == DeformMode_Waypoint) {
      for (const lull::WaypointPath* waypoint_path :
           *deformer_def->waypoint_paths()) {
        Optional<WaypointPath> path = BuildWaypointPath(*waypoint_path);
        if (!path) {
          continue;
        }
        auto it = deformer->paths.emplace(path->path_id, path.value());
        if (!it.second) {
          LOG(DFATAL) << "Path already exists: " << path->path_id;
          continue;
        }
      }
    }
    SetDeformerRecursive(deformed, deformer);
    SetDeformationFunction(e);
  } else if (type == kDeformedHash) {
    const DeformedDef* deformed_def = ConvertDef<DeformedDef>(def);
    std::string path_id;
    if (deformed_def->waypoint_path_id() != nullptr) {
      path_id = deformed_def->waypoint_path_id()->str();
    }
    SetAsDeformed(e, path_id);
  } else {
    LOG(DFATAL) << "Invalid type passed to DeformSystem::Create";
    return;
  }
}

void DeformSystem::SetDeformationFunction(Entity entity) {
  // Whether or not we have a valid deformer at this point, we still set the
  // deformation function on the render system. We do this so that the render
  // system will see the deformation function and defer the mesh creation
  // until the first render call. We only need to set this function one time for
  // each entity.
  registry_->Get<RenderSystem>()->SetDeformationFunction(
      entity, [this, entity](float* data, size_t len, size_t stride) {
        return DeformMesh(entity, data, len, stride);
      });
}

void DeformSystem::SetAsDeformed(Entity entity, string_view path_id) {
  Deformed* deformed = deformed_.Emplace(entity);
  if (!deformed) {
    // If this entity is already deformed then just update its path_id.
    deformed_.Get(entity)->path_id = Hash(path_id);
    return;
  }
  deformed->path_id = Hash(path_id);
  const auto* transform_system = registry_->Get<TransformSystem>();
  const Entity parent = transform_system->GetParent(entity);
  const Deformed* parent_deformed = deformed_.Get(parent);
  if (parent_deformed) {
    SetDeformerRecursive(deformed, deformers_.Get(parent_deformed->deformer));
  }

  SetDeformationFunction(entity);
}

void DeformSystem::Destroy(Entity e) {
  Deformed* deformed = deformed_.Get(e);
  if (deformed) {
    SetDeformerRecursive(deformed, nullptr /* deformer */);
  }
  registry_->Get<RenderSystem>()->SetDeformationFunction(e, nullptr);

  deformers_.Destroy(e);
  deformed_.Destroy(e);
}

bool DeformSystem::IsSetAsDeformed(Entity entity) const {
  return (deformed_.Get(entity) != nullptr);
}

bool DeformSystem::IsDeformed(Entity e) const {
  const auto* deformed = deformed_.Get(e);
  return deformed && deformers_.Get(deformed->deformer);
}

float DeformSystem::GetDeformRadius(Entity e) const {
  const auto* deformed = deformed_.Get(e);
  if (!deformed) {
    return 0;
  }
  const auto* deformer = deformers_.Get(deformed->deformer);
  return deformer ? deformer->radius : 0;
}

DeformMode DeformSystem::GetDeformMode(Entity e) const {
  const auto* deformed = deformed_.Get(e);
  if (!deformed) {
    return DeformMode::DeformMode_None;
  }
  const auto* deformer = deformers_.Get(deformed->deformer);
  return deformer ? deformer->mode : DeformMode::DeformMode_None;
}

const Aabb* DeformSystem::UndeformedBoundingBox(Entity entity) const {
  const auto* deformed = deformed_.Get(entity);
  if (!deformed) {
    return nullptr;
  }
  return &deformed->undeformed_aabb;
}

Optional<DeformSystem::WaypointPath> DeformSystem::BuildWaypointPath(
    const lull::WaypointPath& waypoint_path_def) const {
  if (waypoint_path_def.waypoints() == nullptr ||
      waypoint_path_def.waypoints()->size() == 0) {
    LOG(DFATAL) << "Path missing required field waypoints";
    return Optional<WaypointPath>();
  }
  Optional<mathfu::vec3> parameterization_axis =
      CalculateParameterizationAxis(waypoint_path_def);
  if (!parameterization_axis) {
    LOG(DFATAL) << "Failed to calculate the parameterization axis";
    return Optional<WaypointPath>();
  }

  std::string path_id;
  if (waypoint_path_def.path_id() != nullptr) {
    path_id = waypoint_path_def.path_id()->str();
  }

  WaypointPath waypoint_path;
  waypoint_path.path_id = Hash(path_id.c_str());
  waypoint_path.parameterization_axis = parameterization_axis.value();

  const auto& waypoints = *waypoint_path_def.waypoints();
  for (const lull::Waypoint* waypoint_def : waypoints) {
    DeformSystem::Waypoint waypoint;
    mathfu::vec3 original_position;
    MathfuVec3FromFbVec3(waypoint_def->original_position(), &original_position);
    MathfuVec3FromFbVec3(waypoint_def->remapped_position(),
                         &waypoint.remapped_position);
    MathfuVec3FromFbVec3(waypoint_def->remapped_rotation(),
                         &waypoint.remapped_rotation);
    waypoint_path.waypoints.push_back(waypoint);
    const float parameterized_value = mathfu::vec3::DotProduct(
        original_position, waypoint_path.parameterization_axis);
    waypoint_path.parameterization_values.push_back(parameterized_value);
    if (!waypoint_path.parameterization_values.empty() &&
        parameterized_value < waypoint_path.parameterization_values.back()) {
      LOG(WARNING) << "Waypoint nodes aren't sorted";
    }
  }
  return waypoint_path;
}

void DeformSystem::ApplyDeform(Entity e, const Deformer* deformer) {
  auto& transform_system = *registry_->Get<TransformSystem>();
  if (!deformer || deformer->mode == DeformMode_None) {
    transform_system.SetWorldFromEntityMatrixFunction(e, nullptr);
    return;
  }

  TransformSystem::CalculateWorldFromEntityMatrixFunc world_from_entity_fn;
  switch (deformer->mode) {
    case DeformMode_None:
      break;
    case DeformMode_GlobalCylinder: {
      const float radius = deformer->radius;
      world_from_entity_fn = [this, e, radius](
          const Sqt& local_sqt,
          const mathfu::mat4* world_from_parent_mat) -> mathfu::mat4 {
        const float parent_radius =
            world_from_parent_mat ? GetRadius(*world_from_parent_mat) : 0.f;
        const auto tmp = CalculateCylinderDeformedTransformMatrix(
            local_sqt, parent_radius, radius);
        return world_from_parent_mat ? (*world_from_parent_mat) * tmp : tmp;
      };
      break;
    }
    case DeformMode_CylinderBend: {
      world_from_entity_fn = [this, e](
          const Sqt& local_sqt,
          const mathfu::mat4* world_from_parent_mat) -> mathfu::mat4 {
        return CalculateMatrixCylinderBend(e, local_sqt, world_from_parent_mat);
      };
      break;
    }
    case DeformMode_Waypoint: {
      world_from_entity_fn =
          [this, e, deformer](
              const Sqt& local_sqt,
              const mathfu::mat4* world_from_parent_mat) -> mathfu::mat4 {
        return CalculateWaypointTransformMatrix(e, local_sqt,
                                                world_from_parent_mat);
      };
      break;
    }
  }

  transform_system.SetWorldFromEntityMatrixFunction(e, world_from_entity_fn);
}

void DeformSystem::DeformMesh(Entity e, float* data, size_t len,
                              size_t stride) {
  // There are two scenarios here that need to be accounted for: the first is
  // the nominal case when we have a valid Deformed component, and the second is
  // the legacy case where there is no Deformed component and instead the
  // Deformer is stored in the deformers component pool keyed with this entity.
  Deformed* deformed = deformed_.Get(e);
  if (deformed) {
    const Deformer* deformer = deformers_.Get(deformed->deformer);
    if (deformer != nullptr && deformer->mode == DeformMode_CylinderBend) {
      deformed->undeformed_aabb = GetBoundingBox(data, len, stride);
      CylinderBendDeformMesh(*deformed, *deformer, data, len, stride);
    } else if (deformer != nullptr && deformer->mode == DeformMode_Waypoint) {
      // Waypoint deformation deliberately does not deform mesh
    } else {
      LOG(ERROR) << "Invalid deformer, skipping deformation for entity: " << e;
      return;
    }
  } else {
    const Deformer* deformer = deformers_.Get(e);
    if (deformer != nullptr || deformer->mode != DeformMode_GlobalCylinder) {
      LOG(ERROR) << "Invalid deformer, skipping deformation for entity: " << e;
      return;
    }
    const float current_radius = GetRadius(
        *registry_->Get<TransformSystem>()->GetWorldFromEntityMatrix(e));
    const mathfu::vec3 translation = current_radius * mathfu::kAxisZ3f;
    ApplyDeformation(data, len, stride, [&](const mathfu::vec3& pos) {
      return DeformPoint(pos - translation, deformer->radius) + translation;
    });
  }
}

mathfu::mat4 DeformSystem::CalculateMatrixCylinderBend(
    Entity e, const Sqt& local_sqt,
    const mathfu::mat4* world_from_parent_mat) const {
  const auto* deformed = deformed_.Get(e);
  const auto* deformer =
      (deformed) ? deformers_.Get(deformed->deformer) : nullptr;
  if (!PrepDeformerFromEntityUndeformedSpace(e, local_sqt, deformer,
                                             deformed)) {
    return CalculateTransformMatrixFromParent(local_sqt, world_from_parent_mat);
  }

  const auto& transform_system = *registry_->Get<TransformSystem>();
  return (*transform_system.GetWorldFromEntityMatrix(deformed->deformer)) *
         CalculateCylinderDeformedTransformMatrix(
             deformed->deformer_from_entity_undeformed_space, deformer->radius,
             deformer->clamp_angle);
}

mathfu::mat4 DeformSystem::CalculateWaypointTransformMatrix(
    Entity e, const Sqt& local_sqt,
    const mathfu::mat4* world_from_parent_mat) const {
  const auto* deformed = deformed_.Get(e);
  const auto* deformer =
      (deformed) ? deformers_.Get(deformed->deformer) : nullptr;
  if (!PrepDeformerFromEntityUndeformedSpace(e, local_sqt, deformer,
                                             deformed)) {
    return CalculateTransformMatrixFromParent(local_sqt, world_from_parent_mat);
  }

  auto it = deformer->paths.find(deformed->path_id);
  if (it == deformer->paths.end()) {
    LOG(ERROR) << "Missing deformation path: " << deformed->path_id;
    return CalculateTransformMatrixFromParent(local_sqt, world_from_parent_mat);
  }

  const WaypointPath& path = it->second;
  const Sqt entity_from_root_sqt =
      CalculateSqtFromMatrix(deformed->deformer_from_entity_undeformed_space);
  const float current_point = mathfu::vec3::DotProduct(
      entity_from_root_sqt.translation, path.parameterization_axis);

  size_t min_index;
  size_t max_index;
  float entity_match_percentage;
  FindPositionBetweenPoints(current_point, path.parameterization_values,
                            &min_index, &max_index, &entity_match_percentage);

  const mathfu::vec3 deformed_translation = mathfu::vec3::Lerp(
      path.waypoints[min_index].remapped_position,
      path.waypoints[max_index].remapped_position, entity_match_percentage);

  const mathfu::vec3 deformed_euler_rotation = mathfu::vec3::Lerp(
      path.waypoints[min_index].remapped_rotation,
      path.waypoints[max_index].remapped_rotation, entity_match_percentage);

  const mathfu::quat deformed_rotation = mathfu::quat::FromEulerAngles(
      deformed_euler_rotation * kDegreesToRadians);

  const Sqt deformed_sqt =
      Sqt(deformed_translation, deformed_rotation * local_sqt.rotation,
          local_sqt.scale);
  const mathfu::mat4 deformed_world_from_deformer =
      *registry_->Get<TransformSystem>()->GetWorldFromEntityMatrix(
          deformer->GetEntity());
  return CalculateTransformMatrixFromParent(deformed_sqt,
                                            &deformed_world_from_deformer);
}

void DeformSystem::CylinderBendDeformMesh(const Deformed& deformed,
                                          const Deformer& deformer, float* data,
                                          size_t len, size_t stride) const {
  const TransformSystem& transform_system = *registry_->Get<TransformSystem>();
  const mathfu::mat4* world_from_entity_deformed_space =
      transform_system.GetWorldFromEntityMatrix(deformed.GetEntity());
  const mathfu::mat4* world_from_deformer_deformed_space =
      transform_system.GetWorldFromEntityMatrix(deformer.GetEntity());
  if (!world_from_entity_deformed_space ||
      !world_from_deformer_deformed_space) {
    return;
  }

  // To deform the mesh we first transform the vertices into the deformer root
  // space, which is offset from the deformer itself by the radius along the
  // z-axis. To get back out of root space, we have to use the deformed
  // transforms that we have set on the transform system.
  const float radius = deformer.radius;
  const mathfu::mat4 root_from_entity_undeformed_space =
      mathfu::mat4::FromTranslationVector(-radius * mathfu::kAxisZ3f) *
      deformed.deformer_from_entity_undeformed_space;

  const mathfu::mat4 entity_from_root_deformed_space =
      world_from_entity_deformed_space->Inverse() *
      (*world_from_deformer_deformed_space) *
      mathfu::mat4::FromTranslationVector(radius * mathfu::kAxisZ3f);

  ApplyDeformation(data, len, stride, [&radius,
                                       &root_from_entity_undeformed_space,
                                       &entity_from_root_deformed_space](
                                          const mathfu::vec3& pos) {
    return entity_from_root_deformed_space *
           DeformPoint(root_from_entity_undeformed_space * pos, radius);
  });
}

void DeformSystem::OnParentChanged(const ParentChangedEvent& ev) {
  auto* deformed = deformed_.Get(ev.target);
  if (deformed) {
    // First check if the changed Entity is itself a Deformer.
    const Deformer* deformer = deformers_.Get(ev.target);
    if (!deformer) {
      // If the changed entity is not a Deformer, set its Deformer based on its
      // new parent's deformer.
      auto parent_deformed = deformed_.Get(ev.new_parent);
      if (parent_deformed) {
        deformer = deformers_.Get(parent_deformed->deformer);
      }
    }
    SetDeformerRecursive(deformed, deformer);
  }
}

void DeformSystem::SetDeformerRecursive(Deformed* deformed,
                                        const Deformer* deformer) {
  Entity deformer_entity = kNullEntity;
  if (deformer) {
    deformer_entity = deformer->GetEntity();
  }

  if (deformer_entity != deformed->deformer) {
    deformed->deformer = deformer_entity;
    ApplyDeform(deformed->GetEntity(), deformer);

    const auto& transform_system = *registry_->Get<TransformSystem>();
    const std::vector<Entity>* children =
        transform_system.GetChildren(deformed->GetEntity());
    if (children) {
      for (const auto& child : *children) {
        auto* child_deformed = deformed_.Get(child);
        if (child_deformed) {
          SetDeformerRecursive(child_deformed, deformer);
        }
      }
    }
  }
}

bool DeformSystem::PrepDeformerFromEntityUndeformedSpace(
    const Entity& e, const Sqt& local_sqt, const Deformer* deformer,
    const Deformed* deformed) const {
  if (!deformed) {
    LOG(ERROR) << "Missing deformed, skipping deformation for entity: " << e;
    return false;
  }

  if (!deformer) {
    LOG(ERROR) << "Missing deformer, skipping deformation for entity: " << e;
    return false;
  }

  // When the entity is its own deformer then there is nothing to do.
  if (e == deformed->deformer) {
    deformed->deformer_from_entity_undeformed_space = mathfu::mat4::Identity();
    return false;
  }

  // We cannot use the world_from_parent_mat passed into this function in order
  // to calculated the transform from this entity to the deformer because that
  // matrix has been calculated in post-deformation space. We need the transform
  // in pre-deformation space, and to get it we rely on the chain of
  // deformer_from_entity_undeformed_space matrices cached with the deformed
  // components.
  const auto& transform_system = *registry_->Get<TransformSystem>();
  const Entity parent_entity = transform_system.GetParent(e);
  const Deformed* parent_deformed = deformed_.Get(parent_entity);
  if (!parent_deformed || parent_deformed->deformer == kNullEntity) {
    LOG(ERROR) << "A deformed entity " << e << " has non deformed parent "
               << parent_entity << ". It will not deform.";
    return false;
  }

  deformed->deformer_from_entity_undeformed_space =
      parent_deformed->deformer_from_entity_undeformed_space *
      CalculateTransformMatrix(local_sqt);
  return true;
}

}  // namespace lull
