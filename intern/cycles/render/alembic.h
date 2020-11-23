/*
 * Copyright 2011-2018 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "graph/node.h"
#include "render/attribute.h"
#include "render/procedural.h"
#include "util/util_transform.h"
#include "util/util_vector.h"
#include "util/util_set.h"

//#ifdef WITH_ALEMBIC

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

CCL_NAMESPACE_BEGIN

class Geometry;
class Object;
class Shader;

class AlembicObject : public Node {
 public:
  NODE_DECLARE

  AlembicObject();
  ~AlembicObject();

  NODE_SOCKET_API(ustring, path)
  NODE_SOCKET_API_ARRAY(array<Node *>, used_shaders)

  void set_object(Object *object);
  Object *get_object();

  void load_all_data(const Alembic::AbcGeom::IPolyMeshSchema &schema);

  bool has_data_loaded() const;

  // TODO : this is only for Meshes at the moment

  template <typename T>
  struct DataTimePair {
      double time = 0;
      T data{};
  };

  template <typename T>
  class DataStore {
      vector<DataTimePair<T>> data{};

  public:
      T *data_for_time(double time)
      {
        /* TODO: arrays are emptied */
        if (size() == 1 && std::abs(data[0].time - time) < 1e-6) {
          return &data[0].data;
        }

        for (size_t i = 0; i < size() - 1; ++i) {
          DataTimePair<T> &pair0 = data[i];

          if (std::abs(pair0.time - time) <= 1e-6) {
            return &pair0.data;
          }

          DataTimePair<T> &pair1 = data[i + 1];

          if (std::abs(pair1.time - time) <= 1e-6) {
            return &pair1.data;
          }

          /* see if we are in between samples to avoid issues when the FPS is
           * different than the one for the Alembic archive */
          if (pair0.time <= time && time < pair1.time) {
            return &pair0.data;
          }
        }

        return nullptr;
      }

      void add_data(T data_, double time)
      {
        data.push_back({ time, data_ });
      }

      void add_data(array<T> &data_, double time)
      {
        data.emplace_back();
        data.back().data.steal_data(data_);
        data.back().time = time;
      }

      bool is_constant() const
      {
        return data.size() <= 1;
      }

      size_t size() const
      {
        return data.size();
      }

      void clear()
      {
        data.clear();
      }
  };

  struct CachedData {
      DataStore<array<float3>> vertices;
      DataStore<array<int3>> triangles{};
      DataStore<array<int3>> triangles_loops{};
      DataStore<Transform> transforms{};
      ccl::set<double> dirty_frames{};

      struct CachedAttribute {
          AttributeStandard std;
          AttributeElement element;
          TypeDesc type_desc;
          ustring name;
          DataStore<array<char>> data{};
      };

      vector<CachedAttribute> attributes{};

      void clear()
      {
        vertices.clear();
        triangles.clear();
        triangles_loops.clear();
        transforms.clear();
        dirty_frames.clear();
        attributes.clear();
      }

      void add_dirty_frame(double f)
      {
        dirty_frames.insert(f);
      }

      bool is_dirty_frame(double f)
      {
        return dirty_frames.find(f) != dirty_frames.end();
      }

      CachedAttribute &add_attribute(ustring name)
      {
        for (auto &attr : attributes) {
          if (attr.name == name) {
            return attr;
          }
        }

        auto &attr = attributes.emplace_back();
        attr.name = name;
        return attr;
      }
  };

  Alembic::AbcGeom::IObject iobject;
  Transform xform;

  CachedData &get_cached_data()
  {
    return cached_data;
  }

 private:
  Object *object = nullptr;
  Geometry *geometry = nullptr;

  // runtime data
  bool data_loaded = false;

  CachedData cached_data;

  void read_attribute(const Alembic::AbcGeom::ICompoundProperty &arb_geom_params,
                      const Alembic::AbcGeom::ISampleSelector &iss,
                      const ustring &attr_name);
};

class AlembicProcedural : public Procedural {
 public:
  NODE_DECLARE

  AlembicProcedural();
  ~AlembicProcedural();
  void generate(Scene *scene);

  NODE_SOCKET_API(bool, use_motion_blur)
  NODE_SOCKET_API(ustring, filepath)
  NODE_SOCKET_API(float, frame)
  NODE_SOCKET_API(float, frame_rate)

  array<AlembicObject *> objects;  // todo : Node::set

 private:
  Alembic::AbcGeom::IArchive archive;
  bool objects_loaded = false;

  void load_objects();

  void read_mesh(Scene *scene,
                 AlembicObject *abc_object,
                 Transform xform,
                 Alembic::AbcGeom::IPolyMesh &mesh,
                 Alembic::AbcGeom::Abc::chrono_t frame_time);

  void read_curves(Scene *scene,
                   AlembicObject *abc_object,
                   Transform xform,
                   Alembic::AbcGeom::ICurves &curves,
                   Alembic::AbcGeom::Abc::chrono_t frame_time);
};

CCL_NAMESPACE_END