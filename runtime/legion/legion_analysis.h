/* Copyright 2018 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LEGION_ANALYSIS_H__
#define __LEGION_ANALYSIS_H__

#include "legion/legion_types.h"
#include "legion/legion_utilities.h"
#include "legion/legion_allocation.h"
#include "legion/garbage_collection.h"

namespace Legion {
  namespace Internal {

    /**
     * \struct GenericUser
     * A base struct for tracking the user of a logical region
     */
    struct GenericUser {
    public:
      GenericUser(void) { }
      GenericUser(const RegionUsage &u, const FieldMask &m)
        : usage(u), field_mask(m) { }
    public:
      RegionUsage usage;
      FieldMask field_mask;
    };

    /**
     * \struct LogicalUser
     * A class for representing logical users of a logical 
     * region including the necessary information to
     * register mapping dependences on the user.
     */
    struct LogicalUser : public GenericUser {
    public:
      LogicalUser(void);
      LogicalUser(Operation *o, unsigned id, 
                  const RegionUsage &u, const FieldMask &m);
      LogicalUser(Operation *o, GenerationID gen, unsigned id,
                  const RegionUsage &u, const FieldMask &m);
    public:
      Operation *op;
      unsigned idx;
      GenerationID gen;
      // This field addresses a problem regarding when
      // to prune tasks out of logical region tree data
      // structures.  If no later task ever performs a
      // dependence test against this user, we might
      // never prune it from the list.  This timeout
      // prevents that from happening by forcing a
      // test to be performed whenever the timeout
      // reaches zero.
      int timeout;
#ifdef LEGION_SPY
      UniqueID uid;
#endif
    public:
      static const int TIMEOUT = DEFAULT_LOGICAL_USER_TIMEOUT;
    };

    /**
     * \class VersioningSet
     * A small helper class for tracking collections of 
     * version state objects and their sets of fields
     * This is the same as the above class, but specialized
     * for VersionState objects explicitly
     */
    template<ReferenceSource REF_SRC = LAST_SOURCE_REF>
    class VersioningSet : 
      public LegionHeapify<VersioningSet<REF_SRC> > {
    public:
      class iterator : public std::iterator<std::input_iterator_tag,
                              std::pair<VersionState*,FieldMask> > {
      public:
        iterator(const VersioningSet *_set, 
            std::pair<VersionState*,FieldMask> *_result, bool _single)
          : set(_set), result(_result), single(_single) { }
      public:
        iterator(const iterator &rhs)
          : set(rhs.set), result(rhs.result), single(rhs.single) { }
        ~iterator(void) { }
      public:
        inline iterator& operator=(const iterator &rhs)
          { set = rhs.set; result = rhs.result; 
            single = rhs.single; return *this; }
      public:
        inline bool operator==(const iterator &rhs) const
          { return (set == rhs.set) && (result == rhs.result) && 
                    (single == rhs.single); }
        inline bool operator!=(const iterator &rhs) const
          { return (set != rhs.set) || (result != rhs.result) || 
                    (single != rhs.single); }
      public:
        inline std::pair<VersionState*,FieldMask> operator*(void) 
          { return *result; }
        inline std::pair<VersionState*,FieldMask>* operator->(void)
          { return result; }
        inline iterator& operator++(/*prefix*/void)
          { if (single) result = NULL; 
            else result = set->next(result->first); 
            return *this; }
        inline iterator operator++(/*postfix*/int)
          { iterator copy(*this); 
            if (single) result = NULL; 
            else result = set->next(result->first); 
            return copy; }
      public:
        inline operator bool(void) const
          { return (result != NULL); }
      private:
        const VersioningSet *set;
        std::pair<VersionState*,FieldMask> *result;
        bool single;
      };
    public:
      VersioningSet(void);
      VersioningSet(const VersioningSet &rhs);
      ~VersioningSet(void);
    public:
      VersioningSet& operator=(const VersioningSet &rhs);
    public:
      inline bool empty(void) const 
        { return single && (versions.single_version == NULL); }
      inline const FieldMask& get_valid_mask(void) const 
        { return valid_fields; }
    public:
      const FieldMask& operator[](VersionState *state) const;
    public:
      // Return true if we actually added the state, false if it already existed
      bool insert(VersionState *state, const FieldMask &mask, 
                  ReferenceMutator *mutator = NULL);
      RtEvent insert(VersionState *state, const FieldMask &mask, 
                     Runtime *runtime, RtEvent pre);
      void erase(VersionState *to_erase);
      void clear(void);
      size_t size(void) const;
    public:
      std::pair<VersionState*,FieldMask>* next(VersionState *current) const;
    public:
      void move(VersioningSet &other);
    public:
      iterator begin(void) const;
      inline iterator end(void) const { return iterator(this, NULL, single); }
#ifdef DEBUG_LEGION
    public:
      void sanity_check(void) const;
#endif
    protected:
      // Fun with C, keep these two fields first and in this order
      // so that a 
      // VersioningSet of size 1 looks the same as an entry
      // in the STL Map in the multi-version case, 
      // provides goodness for the iterator
      union {
        VersionState *single_version;
        LegionMap<VersionState*,FieldMask>::aligned *multi_versions;
      } versions;
      // These can be an overapproximation if we have multiple versions
      FieldMask valid_fields;
      bool single;
    };

    // Small helper struct for adding references to versioning set values
    struct VersioningSetRefArgs : public LgTaskArgs<VersioningSetRefArgs> {
    public:
      static const LgTaskID TASK_ID = LG_ADD_VERSIONING_SET_REF_TASK_ID;
    public:
      VersionState *state;
      ReferenceSource kind;
    };

    // Small typedef to define field versions
    typedef LegionMap<VersionID,FieldMask>::aligned FieldVersions;
    /**
     * \class VersionTracker
     * This class provides a single abstract method for getting
     * the version numbers associated with a given node in the 
     * region tree. It is implemented by the VersionInfo class
     * and by the CompositeView class.
     */
    class VersionTracker {
    public:
      virtual bool is_upper_bound_node(RegionTreeNode *node) const = 0;
      virtual void get_field_versions(RegionTreeNode *node, bool split_prev, 
                                      const FieldMask &needed_fields,
                                      FieldVersions &field_versions) = 0;
      virtual void get_advance_versions(RegionTreeNode *node, bool base,
                                        const FieldMask &needed_fields,
                                        FieldVersions &field_versions) = 0;
      virtual void get_split_mask(RegionTreeNode *node, 
                                  const FieldMask &needed_fields,
                                  FieldMask &split) = 0;
      // Pack from the upper bound node down to the target
      // Works with VersionInfo::unpack_version_numbers
      virtual void pack_writing_version_numbers(Serializer &rez) const = 0;
      // Works with VersionInfo::unpack_upper bound node
      virtual void pack_upper_bound_node(Serializer &rez) const = 0;
    };

    /**
     * \class VersionInfo
     * A class for tracking version information about region usage
     */
    class VersionInfo : public VersionTracker {
    public:
      VersionInfo(void);
      VersionInfo(const VersionInfo &rhs);
      virtual ~VersionInfo(void);
    public:
      VersionInfo& operator=(const VersionInfo &rhs);
    public:
      void record_split_fields(RegionTreeNode *node, const FieldMask &split,
                               unsigned offset = 0);
      void add_current_version(VersionState *state, 
                               const FieldMask &state_mask, bool path_only);
      void add_advance_version(VersionState *state, 
                               const FieldMask &state_mask, bool path_only);
    public:
      inline bool is_upper_bound_set(void) const 
        { return (upper_bound_node != NULL); }
      virtual bool is_upper_bound_node(RegionTreeNode *node) const
        { return (node == upper_bound_node); }
      inline RegionTreeNode* get_upper_bound_node(void) const 
        { return upper_bound_node; }
      inline size_t get_depth(void) const 
#ifdef DEBUG_LEGION
        { assert(!physical_states.empty()); return physical_states.size() - 1; }
#else
        { return physical_states.size() - 1; }
#endif
      void set_upper_bound_node(RegionTreeNode *node);
      bool has_physical_states(void) const; 
    public:
      // The copy through parameter is useful for mis-speculated
      // operations that still need to copy state from one 
      // version number to the next even though they didn't
      // modify the physical state object
      void apply_mapping(std::set<RtEvent> &applied_conditions,
                         bool copy_through = false);
      void resize(size_t max_depth);
      void resize(size_t path_depth, HandleType req_handle,
                  ProjectionFunction *function);
      void clear(void);
      void sanity_check(unsigned depth);
      // Cloning logical state for internal opeations
      void clone_logical(const VersionInfo &rhs, const FieldMask &mask,
                         RegionTreeNode *to_node);
      void copy_to(VersionInfo &rhs);
      // Cloning information for virtual mappings
      void clone_to_depth(unsigned depth, const FieldMask &mask,
                          InnerContext *context, VersionInfo &target_info,
                          std::set<RtEvent> &ready_events) const;
    public:
      PhysicalState* find_physical_state(RegionTreeNode *node); 
      const FieldMask& get_split_mask(unsigned depth) const;
      virtual void get_split_mask(RegionTreeNode *node, 
                                  const FieldMask &needed_fields,
                                  FieldMask &split);
      virtual void get_field_versions(RegionTreeNode *node, bool split_prev,
                                      const FieldMask &needed_fields,
                                      FieldVersions &field_versions);
      virtual void get_advance_versions(RegionTreeNode *node, bool base,
                                        const FieldMask &needed_fields,
                                        FieldVersions &field_versions);
      virtual void pack_writing_version_numbers(Serializer &rez) const;
    public:
      void pack_version_info(Serializer &rez) const;
      void unpack_version_info(Deserializer &derez, Runtime *runtime,
                               std::set<RtEvent> &ready_events);
      void pack_version_numbers(Serializer &rez) const;
      void unpack_version_numbers(Deserializer &derez,RegionTreeForest *forest);
    public:
      virtual void pack_upper_bound_node(Serializer &rez) const;
      void unpack_upper_bound_node(Deserializer &derez, 
                                   RegionTreeForest *forest);
    public:
      // Used by control replication for grabbing base advance states
      void capture_base_advance_states(
          LegionMap<DistributedID,FieldMask>::aligned &base_states) const;
    protected:
      RegionTreeNode                   *upper_bound_node;
      // All of these are indexed by depth in the region tree
      std::vector<PhysicalState*>      physical_states;
      std::vector<FieldVersions>       field_versions;
      LegionVector<FieldMask>::aligned split_masks;
    };

    /**
     * \class Restriction
     * A class for tracking restrictions that occur as part of
     * relaxed coherence and with tracking external resources
     */
    class Restriction : public LegionHeapify<Restriction> {
    public:
      Restriction(RegionNode *node);
      Restriction(const Restriction &rhs);
      ~Restriction(void);
    public:
      Restriction& operator=(const Restriction &rhs);
    public:
      void add_restricted_instance(PhysicalManager *inst, 
                                   const FieldMask &restricted_fields);
    public:
      void find_restrictions(RegionTreeNode *node, 
                             FieldMask &possibly_restricted,
                             RestrictInfo &restrict_info) const;
      bool matches(DetachOp *op, RegionNode *node,
                   FieldMask &remaining_fields); 
      void remove_restricted_fields(FieldMask &remaining_fields) const;
    public:
      void add_acquisition(AcquireOp *op, RegionNode *node,
                           FieldMask &remaining_fields);
      void remove_acquisition(ReleaseOp *op, RegionNode *node,
                              FieldMask &remaining_fields);
    public:
      void add_restriction(AttachOp *op, RegionNode *node,
                PhysicalManager *manager, FieldMask &remaining_fields);
      void remove_restriction(DetachOp *op, RegionNode *node,
                              FieldMask &remaining_fields);

    public:
      const RegionTreeID tree_id;
      RegionNode *const local_node;
    protected:
      FieldMask restricted_fields;
      std::set<Acquisition*> acquisitions;
      // We only need garbage collection references on these
      // instances because we know one of two things is always
      // true: either they are attached files so they aren't 
      // subject to memories in which garbage collection will
      // occur, or they are simultaneous restricted, so that
      // enclosing context of the parent task has a valid 
      // reference to them so there is no need for us to 
      // have a valid reference.
      // Same in RestrictInfo
      LegionMap<PhysicalManager*,FieldMask>::aligned instances;
    };

    /**
     * \class Acquisition
     * A class for tracking when restrictions are relaxed by
     * explicit acquisitions of a region
     */
    class Acquisition : public LegionHeapify<Acquisition> {
    public:
      Acquisition(RegionNode *node, const FieldMask &acquired_fields);
      Acquisition(const Acquisition &rhs);
      ~Acquisition(void);
    public:
      Acquisition& operator=(const Acquisition &rhs);
    public:
      void find_restrictions(RegionTreeNode *node, 
                             FieldMask &possibly_restricted,
                             RestrictInfo &restrict_info) const;
      bool matches(ReleaseOp *op, RegionNode *node, 
                   FieldMask &remaining_fields);
      void remove_acquired_fields(FieldMask &remaining_fields) const;
    public:
      void add_acquisition(AcquireOp *op, RegionNode *node,
                           FieldMask &remaining_fields);
      void remove_acquisition(ReleaseOp *op, RegionNode *node,
                              FieldMask &remaining_fields);
    public:
      void add_restriction(AttachOp *op, RegionNode *node,
                 PhysicalManager *manager, FieldMask &remaining_fields);
      void remove_restriction(DetachOp *op, RegionNode *node,
                              FieldMask &remaining_fields);
    public:
      RegionNode *const local_node;
    protected:
      FieldMask acquired_fields;
      std::set<Restriction*> restrictions;
    };

    /**
     * \struct LogicalTraceInfo
     * Information about tracing needed for logical
     * dependence analysis.
     */
    struct LogicalTraceInfo {
    public:
      LogicalTraceInfo(bool already_tr,
                       LegionTrace *tr,
                       unsigned idx,
                       const RegionRequirement &r);
    public:
      bool already_traced;
      LegionTrace *trace;
      unsigned req_idx;
      const RegionRequirement &req;
    };

    /**
     * \struct PhysicalTraceInfo
     */
    struct PhysicalTraceInfo {
    public:
      explicit PhysicalTraceInfo(Operation *op, bool initialize = true);
      PhysicalTraceInfo(Operation *op, Memoizable *memo);
    public:
      void record_merge_events(ApEvent &result, ApEvent e1, ApEvent e2) const;
      void record_merge_events(ApEvent &result, ApEvent e1, 
                               ApEvent e2, ApEvent e3) const;
      void record_merge_events(ApEvent &result, 
                               const std::set<ApEvent> &events) const;
      void record_op_sync_event(ApEvent &result) const;
    public:
      void record_issue_copy(ApEvent &result, RegionNode *node,
                             const std::vector<CopySrcDstField>& src_fields,
                             const std::vector<CopySrcDstField>& dst_fields,
                             ApEvent precondition,
                             PredEvent predicate_guard,
                             IndexTreeNode *intersect,
                             IndexSpaceExpression *mask,
                             ReductionOpID redop,
                             bool reduction_fold) const;
      void record_issue_fill(ApEvent &result, RegionNode *node,
                             const std::vector<CopySrcDstField> &fields,
                             const void *fill_buffer, size_t fill_size,
                             ApEvent precondition,
                             PredEvent predicate_guard,
#ifdef LEGION_SPY
                             UniqueID fill_uid,
#endif
                             IndexTreeNode *intersect,
                             IndexSpaceExpression *mask) const;
      void record_empty_copy(CompositeView *view,
                             const FieldMask &copy_mask,
                             MaterializedView *dst) const;
    public:
      Operation *const op;
      PhysicalTemplate *const tpl;
      const bool recording;
    };

    /**
     * \class ProjectionInfo
     * Projection information for index space requirements
     */
    class ProjectionInfo {
    public:
      ProjectionInfo(void)
        : projection(NULL), projection_type(SINGULAR),
          projection_space(NULL), sharding_function(NULL),
          dirty_reduction(false) { }
      ProjectionInfo(Runtime *runtime, const RegionRequirement &req,
                     IndexSpace launch_space, ShardingFunction *func = NULL);
    public:
      inline bool is_projecting(void) const { return (projection != NULL); }
      inline const LegionMap<ProjectionEpochID,FieldMask>::aligned&
        get_projection_epochs(void) const { return projection_epochs; }
      inline bool is_dirty_reduction(void) const { return dirty_reduction; }
      inline void set_dirty_reduction(void) { dirty_reduction = true; }
      void record_projection_epoch(ProjectionEpochID epoch,
                                   const FieldMask &epoch_mask);
      void clear(void);
    public:
      void pack_info(Serializer &rez) const;
      void unpack_info(Deserializer &derez, Runtime *runtime,
          const RegionRequirement &req, IndexSpaceNode* launch_node);
    public:
      void pack_epochs(Serializer &rez) const;
      void unpack_epochs(Deserializer &derez);
    public:
      ProjectionFunction *projection;
      ProjectionType projection_type;
      IndexSpaceNode *projection_space;
      ShardingFunction *sharding_function;
    protected:
      // Use this information to deduplicate between different points
      // trying to advance information for the same projection epoch
      LegionMap<ProjectionEpochID,FieldMask>::aligned projection_epochs;
    protected:
      // Track whether this is a dirty reduction, which means that we
      // know that an advance has already been done by a previous write
      // so that we know we don't have do an advance to get our reduction
      // registered with the parent version state. If it is not a dirty
      // reduction then we have to do the extra advance to get the 
      // reduction registered with parent VersionState object
      bool dirty_reduction;
    };

    /**
     * \struct PhysicalUser
     * A class for representing physical users of a logical
     * region including necessary information to 
     * register execution dependences on the user.
     */
    struct PhysicalUser : public Collectable, 
                          public LegionHeapify<PhysicalUser> {
    public:
      static const AllocationType alloc_type = PHYSICAL_USER_ALLOC;
    public:
      PhysicalUser(void);
      PhysicalUser(const RegionUsage &u, LegionColor child, 
                   UniqueID op_id, unsigned index, IndexSpaceExpression *expr);
      PhysicalUser(const PhysicalUser &rhs);
      ~PhysicalUser(void);
    public:
      PhysicalUser& operator=(const PhysicalUser &rhs);
    public:
      void pack_user(Serializer &rez, AddressSpaceID target);
      static PhysicalUser* unpack_user(Deserializer &derez, bool add_reference,
                               RegionTreeForest *forest, AddressSpaceID source);
    public:
      RegionUsage usage;
      LegionColor child;
      UniqueID op_id;
      unsigned index; // region requirement index
      IndexSpaceExpression *expr;
    };  

    /**
     * \struct TraversalInfo
     */
    struct TraversalInfo : public PhysicalTraceInfo {
    public:
      TraversalInfo(ContextID ctx, const PhysicalTraceInfo &info, unsigned idx,
                    const RegionRequirement &req, VersionInfo &version_info,
                    const FieldMask &traversal_mask, 
                    std::set<RtEvent> &map_applied_events);
    public:
      void pack(Serializer &rez) const;
    public:
      const ContextID ctx;
      const unsigned index;
      const RegionRequirement &req;
      VersionInfo &version_info;
      const FieldMask traversal_mask;
      const UniqueID context_uid;
      std::set<RtEvent> &map_applied_events;
      ContextID logical_ctx;
    };
    
    /**
     * \struct RemoteTraversalInfo
     */
    struct RemoteTraversalInfo : public TraversalInfo, 
      public LegionHeapify<RemoteTraversalInfo> {
    public:
      RemoteTraversalInfo(RemoteOp *remote_op, unsigned idx,
          const RegionRequirement &r, const FieldMask &mask,
          UniqueID ctx_uid, RtUserEvent remote_applied);
      ~RemoteTraversalInfo(void);
    protected:
      const RtUserEvent remote_applied;
      VersionInfo dummy_version_info;
      std::set<RtEvent> applied_events;
    public:
      static RemoteTraversalInfo* unpack(Deserializer &derez, Runtime *runtime);
    };

    /**
     * \class WriteMasks
     * This is an instantiation of FieldMaskSet with an 
     * IndexSpaceExpression to delineate a set of writes which
     * we no longer need to perform, think of it like a photographic
     * negative that prevents writing in some cases. Even though
     * this has the same base type as WriteSet (see below) we have
     * WriteSet inherit form WriteMask, which allows a write set to
     * be treated as a WriteMask, but never the other direction.
     * Hopefully this keeps us from being confused and the type
     * system will check things for us.
     */
    class WriteMasks : public FieldMaskSet<IndexSpaceExpression> {
    public:
      // Merge two write masks into one and deduplicate where necessary
      void merge(const WriteMasks &other);
    };

    /**
     * \class WriteSet
     * This is an instantiation of FieldMaskSet with an 
     * IndexSpaceExpression to track the set of writes which have
     * been performed. This is in contrast to a WriteMask set which
     * is the set of things for which we are not performing writes
     * (see above). Even though the types are identical we have
     * WriteSet inherit from WriteMask so a WriteSet can be used
     * as a WriteMask, but never the other way around.
     */
    class WriteSet : public WriteMasks {
    };

    /**
     * \struct ChildState
     * Tracks the which fields have open children
     * and then which children are open for each
     * field. We also keep track of the children
     * that are in the process of being closed
     * to avoid races on two different operations
     * trying to close the same child.
     */
    struct ChildState {
    public:
      ChildState(void) { }
      ChildState(const FieldMask &m)
        : valid_fields(m) { }
      ChildState(const ChildState &rhs) 
        : valid_fields(rhs.valid_fields),
          open_children(rhs.open_children) { }
    public:
      ChildState& operator=(const ChildState &rhs)
      {
        valid_fields = rhs.valid_fields;
        open_children = rhs.open_children;
        return *this;
      }
    public:
      FieldMask valid_fields;
      LegionMap<LegionColor,FieldMask>::aligned open_children;
    };

    /**
     * \struct ProjectionSummary
     * A small helper class that tracks the triple that 
     * uniquely defines a set of region requirements
     * for a projection operation
     */
    struct ProjectionSummary {
    public:
      ProjectionSummary(void);
      ProjectionSummary(IndexSpaceNode *is, 
                        ProjectionFunction *p, 
                        ShardingFunction *s);
      ProjectionSummary(const ProjectionInfo &info);
    public:
      bool operator<(const ProjectionSummary &rhs) const;
      bool operator==(const ProjectionSummary &rhs) const;
      bool operator!=(const ProjectionSummary &rhs) const;
    public:
      void pack_summary(Serializer &rez) const;
      static ProjectionSummary unpack_summary(Deserializer &derez,
                                              RegionTreeForest *context);
    public:
      IndexSpaceNode *domain;
      ProjectionFunction *projection;
      ShardingFunction *sharding;
    };

    /**
     * \struct ShardingSummary
     * An extension of the ProjectionSummary that is also used
     * to track the region tree node origin of the summary and
     * also be capable of inverting the projection functions so
     * we can find the needed set of shards for communcation.
     */
    struct ShardingSummary : public ProjectionSummary {
    public:
      ShardingSummary(const ProjectionSummary &rhs, RegionTreeNode *node);
      ShardingSummary(const ShardingSummary &rhs);
      ~ShardingSummary(void);
    public:
      ShardingSummary& operator=(const ShardingSummary &rhs); 
    public:
      void pack_summary(Serializer &rez) const;
      static ShardingSummary* unpack_summary(Deserializer &derez,
               RegionTreeForest *forest, InnerContext *context);
    public:
      RegionTreeNode *const node;
    };

    /**
     * \struct CompositeViewSummary
     * This is a structure for holding all the summary data for 
     * constructing a composite view. Specifically it has the 
     * set of fields which are completely written for the view
     * as well as a WriteSet for any partial writes. If the view
     * is being constructed in a control-replicated context then
     * we also capture the sharding summary objects for the view.
     */
    struct CompositeViewSummary {
    public:
      CompositeViewSummary(void);
      CompositeViewSummary(const FieldMask &complete, WriteSet &partial);
      CompositeViewSummary(const FieldMask &complete, WriteSet &partial,
                           FieldMaskSet<ShardingSummary> &writes,
                           FieldMaskSet<ShardingSummary> &reduces);
      CompositeViewSummary(CompositeViewSummary &rhs);
      ~CompositeViewSummary(void);
    public:
      CompositeViewSummary& operator=(const CompositeViewSummary &rhs);
    public:
      void swap(CompositeViewSummary &rhs);
      void clear(void);
      void pack(Serializer &rez, AddressSpaceID target) const;
      void unpack(Deserializer &derez, RegionTreeForest *forest, 
                  AddressSpaceID source, InnerContext *context);
    public:
      FieldMask complete_writes;
      WriteSet partial_writes;
    public:
      // Control replicated contexts only
      FieldMaskSet<ShardingSummary> write_projections;
      FieldMaskSet<ShardingSummary> reduce_projections;
    };

    /**
     * \struct FieldState
     * Track the field state more accurately
     * for logical traversals to figure out 
     * which tasks can run in parallel.
     */
    struct FieldState : public ChildState {
    public:
      FieldState(void);
      FieldState(const GenericUser &u, const FieldMask &m, 
                 LegionColor child);
      FieldState(const RegionUsage &u, const FieldMask &m,
                 ProjectionFunction *proj, IndexSpaceNode *proj_space, 
                 ShardingFunction *sharding_function, RegionTreeNode *node,
                 bool dirty_reduction = false);
    public:
      inline bool is_projection_state(void) const 
        { return (open_state >= OPEN_READ_ONLY_PROJ); } 
    public:
      bool overlaps(const FieldState &rhs) const;
      bool projections_match(const FieldState &rhs) const;
      void merge(const FieldState &rhs, RegionTreeNode *node);
    public:
      bool can_elide_close_operation(const ProjectionInfo &info,
                     RegionTreeNode *node, bool reduction) const;
      void record_projection_summary(const ProjectionInfo &info,
                                     RegionTreeNode *node);
    protected:
      bool expensive_elide_test(const ProjectionInfo &info,
                RegionTreeNode *node, bool reduction) const;
    public:
      void print_state(TreeStateLogger *logger, 
                       const FieldMask &capture_mask,
                       RegionNode *node) const;
      void print_state(TreeStateLogger *logger, 
                       const FieldMask &capture_mask,
                       PartitionNode *node) const;
    public:
      OpenState open_state;
      ReductionOpID redop;
      std::set<ProjectionSummary> projections;
      unsigned rebuild_timeout;
      bool disjoint_shallow;
    };

    /**
     * \class ProjectionEpoch
     * This class captures the set of projection functions
     * and domains that have performed in current open
     * projection epoch
     */
    class ProjectionEpoch : public LegionHeapify<ProjectionEpoch> {
    public:
      static const ProjectionEpochID first_epoch = 1;
    public:
      ProjectionEpoch(ProjectionEpochID epoch_id,
                      const FieldMask &mask);
      ProjectionEpoch(const ProjectionEpoch &rhs);
      ~ProjectionEpoch(void);
    public:
      ProjectionEpoch& operator=(const ProjectionEpoch &rhs);
    public:
      void insert_write(ProjectionFunction *function, IndexSpaceNode *space,
                        ShardingFunction *sharding_function);
      void insert_reduce(ProjectionFunction *function, IndexSpaceNode *space,
                         ShardingFunction *sharding_function);
      void record_closed_projections(LogicalCloser &closer, 
                                     RegionTreeNode *node,
                                     const FieldMask &closing_mask) const;
    public:
      const ProjectionEpochID epoch_id;
      FieldMask valid_fields;
    public:
      std::set<ProjectionSummary> write_projections;
      std::set<ProjectionSummary> reduce_projections;
    };

    /**
     * \class ProjectionTree
     * This is a tree that stores the summary of a region
     * tree that is accessed by an index launch and which
     * node owns the leaves for in the case of control replication
     */
    class ProjectionTree {
    public:
      ProjectionTree(IndexTreeNode *source,
                     ShardID owner_shard = 0);
      ProjectionTree(const ProjectionTree &rhs);
      ~ProjectionTree(void);
    public:
      ProjectionTree& operator=(const ProjectionTree &rhs);
    public:
      void add_child(ProjectionTree *child);
      bool dominates(const ProjectionTree *other) const;
      bool disjoint(const ProjectionTree *other) const;
      bool all_same_shard(ShardID other_shard) const;
    public:
      IndexTreeNode *const node;
      const ShardID owner_shard;
      std::map<IndexTreeNode*,ProjectionTree*> children;
    };

    /**
     * \class LogicalState
     * Track all the information about the current state
     * of a logical region from a given context. This
     * is effectively all the information at the analysis
     * wavefront for this particular logical region.
     */
    class LogicalState : public LegionHeapify<LogicalState> {
    public:
      static const AllocationType alloc_type = CURRENT_STATE_ALLOC;
    public:
      LogicalState(RegionTreeNode *owner, ContextID ctx);
      LogicalState(const LogicalState &state);
      ~LogicalState(void);
    public:
      LogicalState& operator=(const LogicalState &rhs);
    public:
      inline void keep_dirty_fields(FieldMask &to_keep) const
      {
        FieldMask dirty_fields = write_fields | reduction_fields;
        if (!partial_writes.empty())
          dirty_fields |= partial_writes.get_valid_mask();
        to_keep &= dirty_fields;
      }
      inline void filter_dirty_fields(FieldMask &to_filter) const
      {
        FieldMask dirty_fields = write_fields | reduction_fields;
        if (!partial_writes.empty())
          dirty_fields |= partial_writes.get_valid_mask();
        to_filter -= dirty_fields;
      }
      inline void update_write_fields(const FieldMask &update)
      {
        write_fields |= update;
        // we can also filter out any partial writes once we
        // get a write at this level too
        if (partial_writes.empty() ||
            (partial_writes.get_valid_mask() * update))
          return;
        partial_writes.filter(update);
      }
    public:
      void check_init(void);
      void clear_logical_users(void);
      void reset(void);
      void clear_deleted_state(const FieldMask &deleted_mask);
    public:
      void advance_projection_epochs(const FieldMask &advance_mask);
      void capture_projection_epochs(FieldMask capture_mask,
                                     ProjectionInfo &info);
      void update_write_projection_epochs(FieldMask update_mask,
                                          const LogicalUser &user,
                                          const ProjectionInfo &info);
      void update_reduce_projection_epochs(FieldMask update_mask,
                                           const ProjectionInfo &info);
      void find_projection_writes(FieldMask mask,
                                  FieldMask &complete_writes,
                                  WriteSet &partial_writes) const;
      void record_closed_projections(LogicalCloser &closer,
                                     const FieldMask &closing_mask) const;
    public:
      RegionTreeNode *const owner;
    public:
      LegionList<FieldState,
                 LOGICAL_FIELD_STATE_ALLOC>::track_aligned field_states;
      LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned 
                                                            curr_epoch_users;
      LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned 
                                                            prev_epoch_users;
    public:
      // Fields which we know have been mutated below in the region tree
      FieldMask dirty_below;
      // Fields that we know have been written at the current level
      // (reductions don't count, we want to know they were actually written)
      FieldMask write_fields;
      // Furthermore keep track of any partial writes that we see,
      // either from projection writes or from close operations
      WriteSet partial_writes;
      // Keep track of which fields we've done a reduction to here
      FieldMask reduction_fields;
      LegionMap<ReductionOpID,FieldMask>::aligned outstanding_reductions;
    public:
      // Keep track of the current projection epoch for each field
      std::list<ProjectionEpoch*> projection_epochs;
      // Also keep track of any complete projection writes that we've done
      FieldMask projection_write_fields;
      WriteSet projection_partial_writes;
    };

    typedef DynamicTableAllocator<LogicalState,10,8> LogicalStateAllocator;

    /**
     * \class LogicalCloser
     * This structure helps keep track of the state
     * necessary for performing a close operation
     * on the logical region tree.
     */
    class LogicalCloser {
    public:
      LogicalCloser(ContextID ctx, const LogicalUser &u, RegionTreeNode *root, 
                    bool validates, bool captures, bool replicate_context);
      LogicalCloser(const LogicalCloser &rhs);
      ~LogicalCloser(void);
    public:
      LogicalCloser& operator=(const LogicalCloser &rhs);
    public:
      inline bool has_close_operations(void) const 
        { return (!!normal_close_mask) || (!!read_only_close_mask) ||
                  (!!flush_only_close_mask) || (!!disjoint_close_mask); }
      // Record normal closes like this
      void record_close_operation(const FieldMask &mask);
      void record_projection_close(const FieldMask &mask, LogicalState &state,
                                   bool disjoint_close = false);
      void record_overwriting_close(const FieldMask &mask, bool projection);
      void record_read_only_close(const FieldMask &mask, bool projection);
      void record_flush_only_close(const FieldMask &mask);
      void record_closed_user(const LogicalUser &user, 
                              const FieldMask &mask, bool read_only);
      void record_write_projection(const ProjectionSummary &summary,
                                   RegionTreeNode *node, 
                                   const FieldMask &summary_mask);
      void record_reduce_projection(const ProjectionSummary &summary,
                                   RegionTreeNode *node, 
                                   const FieldMask &summary_mask);
#ifndef LEGION_SPY
      void pop_closed_user(bool read_only);
#endif
      void initialize_close_operations(LogicalState &state, 
                                       Operation *creator,
                                       const VersionInfo &version_info,
                                       const LogicalTraceInfo &trace_info);
      void perform_dependence_analysis(const LogicalUser &current,
                                       const FieldMask &open_below,
             LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &cusers,
             LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned &pusers);
      void begin_close_children(const FieldMask &closing_mask,
                                RegionTreeNode *closing_node,
                                const LogicalState &state);
      void end_close_children(FieldMask closed_mask,
                              RegionTreeNode *closed_node);
      void update_close_writes(const FieldMask &closing_mask,
                               RegionTreeNode *closing_node,
                               const LogicalState &state);
      void update_state(LogicalState &state);
      void register_close_operations(
              LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &users);
    protected:
      void register_dependences(CloseOp *close_op, 
                                const LogicalUser &close_user,
                                const LogicalUser &current, 
                                const FieldMask &open_below,
             LegionList<LogicalUser,CLOSE_LOGICAL_ALLOC>::track_aligned &husers,
             LegionList<LogicalUser,LOGICAL_REC_ALLOC>::track_aligned &ausers,
             LegionList<LogicalUser,CURR_LOGICAL_ALLOC>::track_aligned &cusers,
             LegionList<LogicalUser,PREV_LOGICAL_ALLOC>::track_aligned &pusers);
    public:
      ContextID ctx;
      const LogicalUser &user;
      RegionTreeNode *const root_node;
      const bool validates;
      const bool capture_users;
      const bool replicate_context;
      LegionList<LogicalUser,CLOSE_LOGICAL_ALLOC>::track_aligned 
                                                      normal_closed_users;
      LegionList<LogicalUser,CLOSE_LOGICAL_ALLOC>::track_aligned 
                                                      read_only_closed_users;
    protected:
      FieldMask normal_close_mask;
      FieldMask read_only_close_mask;
      FieldMask flush_only_close_mask;
      // Read-only closes because we're overwriting without reading
      FieldMask overwriting_close_mask;
      // Closes for which we are actually closing up individual children
      FieldMask disjoint_close_mask;
      // Fields which closed up a projection operation from this level 
      FieldMask closed_projections;
      // Fields that we did complete writes to from for this close operation
      FieldMask complete_writes;
    protected:
      // Use these for computing the close summaries of what has been written
      LegionDeque<FieldMaskSet<RegionTreeNode> >::aligned written_children;
      LegionDeque<WriteSet>::aligned partial_writes;
      LegionDeque<FieldMask>::aligned written_above;
    protected:
      // At most we will ever generate four close operations at a node
      InterCloseOp *normal_close_op;
      IndexCloseOp *index_close_op;
      ReadCloseOp *read_only_close_op;
      InterCloseOp *flush_only_close_op;
    protected:
      // Cache the generation IDs so we can kick off ops before adding users
      GenerationID normal_close_gen;
      GenerationID read_only_close_gen;
      GenerationID flush_only_close_gen;
    protected:
      // These are only used for control-replicated contexts 
      FieldMaskSet<ShardingSummary> write_projections;
      FieldMaskSet<ShardingSummary> reduce_projections;
    }; 

    /**
     * \class PhysicalState
     * A physical state is a temporary buffer for holding a merged
     * group of version state objects which can then be used by 
     * physical traversal routines. Physical state objects track
     * the version state objects that they use and remove references
     * to them when they are done.
     */
    class PhysicalState : public LegionHeapify<PhysicalState> {
    public:
      static const AllocationType alloc_type = PHYSICAL_STATE_ALLOC;
      typedef VersioningSet<PHYSICAL_STATE_REF> PhysicalVersions;
    public:
      PhysicalState(RegionTreeNode *node, bool path_only);
      PhysicalState(const PhysicalState &rhs);
      ~PhysicalState(void);
    public:
      PhysicalState& operator=(const PhysicalState &rhs);
    public:
      void pack_physical_state(Serializer &rez);
      void unpack_physical_state(Deserializer &derez, Runtime *runtime,
                                 std::set<RtEvent> &ready_events);
    public:
      void add_version_state(VersionState *state, const FieldMask &mask);
      void add_advance_state(VersionState *state, const FieldMask &mask);
    public:
      inline bool is_captured(void) const { return captured; }
      void capture_state(void);
      inline bool has_advance_states(void) const 
        { return (!advance_states.empty()); } 
      void apply_state(std::set<RtEvent> &applied_conditions) const; 
      inline const PhysicalVersions& get_advance_states(void) const
        { return advance_states; }
    public:
      void filter_composite_mask(FieldMask &composite_mask);
      void capture_composite_root(CompositeView *composite_view,
        const FieldMask &closed_mask, ReferenceMutator *mutator,
        const LegionMap<LogicalView*,FieldMask>::aligned &valid_above);
    public:
      PhysicalState* clone(void) const;
      void clone_to(const FieldMask &version_mask, const FieldMask &split_mask,
                    InnerContext *context, VersionInfo &target_info,
                    std::set<RtEvent> &ready_events) const;
    public:
      void print_physical_state(const FieldMask &capture_mask,
                                TreeStateLogger *logger);
    public:
      RegionTreeNode *const node;
      const bool path_only;
    public:
      // Fields which have dirty data
      FieldMask dirty_mask;
      // Fields with outstanding reductions
      FieldMask reduction_mask;
      // The valid instance views
      LegionMap<LogicalView*, FieldMask,
                VALID_VIEW_ALLOC>::track_aligned valid_views;
      // The valid reduction veiws
      LegionMap<ReductionView*, FieldMask,
                VALID_REDUCTION_ALLOC>::track_aligned reduction_views;
    protected:
      PhysicalVersions version_states;
      PhysicalVersions advance_states;
    protected:
      bool captured;
    };

    /**
     * \class VersionManager
     * The VersionManager class tracks the current version state
     * objects for a given region tree node in a specific context
     * VersionManager objects are either an owner or remote. 
     * The owner tracks the set of remote managers and invalidates
     * them whenever changes occur to the version state.
     * Owners are assigned by the enclosing task context using
     * a first-touch policy. The first node to ask to be an owner
     * for a given logical region or partition will be assigned
     * to be the owner.
     */
    class VersionManager : public LegionHeapify<VersionManager> {
    public:
      struct ProjectionEpoch {
      public:
        ProjectionEpoch(void) : logical_ctx_id(0), epoch_id(0) { }
        ProjectionEpoch(UniqueID logical_ctx, ProjectionEpochID epoch)
          : logical_ctx_id(logical_ctx), epoch_id(epoch) { }
      public:
        inline bool operator==(const ProjectionEpoch &rhs) const
          { return ((logical_ctx_id == rhs.logical_ctx_id) && 
                    (epoch_id == rhs.epoch_id)); }
        inline bool operator<(const ProjectionEpoch &rhs) const
          {
            if (logical_ctx_id < rhs.logical_ctx_id) return true;
            if (logical_ctx_id > rhs.logical_ctx_id) return false;
            return (epoch_id < rhs.epoch_id);
          }
      public:
        UniqueID logical_ctx_id;
        ProjectionEpochID epoch_id;
      };
      struct DirtyUpdateArgs : public LgTaskArgs<DirtyUpdateArgs> {
      public:
        static const LgTaskID TASK_ID = LG_VERSION_STATE_CAPTURE_DIRTY_TASK_ID;
      public:
        VersionState *previous;
        VersionState *target;
        FieldMask *capture_mask;
      };
      struct PendingAdvanceArgs : public LgTaskArgs<PendingAdvanceArgs> {
      public:
        static const LgTaskID TASK_ID = 
          LG_VERSION_STATE_PENDING_ADVANCE_TASK_ID;
      public:
        VersionManager *proxy_this;
        RtEvent to_reclaim;
      };
    public:
      static const AllocationType alloc_type = VERSION_MANAGER_ALLOC;
      static const VersionID init_version = 1;
    public:
      typedef VersioningSet<VERSION_MANAGER_REF> ManagerVersions;
    public:
      VersionManager(RegionTreeNode *node, ContextID ctx); 
      VersionManager(const VersionManager &manager);
      ~VersionManager(void);
    public:
      VersionManager& operator=(const VersionManager &rhs);
    public:
      void reset(void);
    public:
      void initialize_state(ApEvent term_event,
                            const RegionUsage &usage,
                            const FieldMask &user_mask,
                            const InstanceSet &targets,
                            InnerContext *context, unsigned init_index,
                            const std::vector<LogicalView*> &corresponding,
                            std::set<RtEvent> &applied_events);
    public:
      void record_current_versions(const FieldMask &version_mask,
                                   FieldMask &unversioned_mask,
                                   InnerContext *context,
                                   Operation *op, unsigned index,
                                   const RegionUsage &usage,
                                   VersionInfo &version_info,
                                   std::set<RtEvent> &ready_events);
      void record_advance_versions(const FieldMask &version_mask,
                                   InnerContext *context,
                                   VersionInfo &version_info,
                                   std::set<RtEvent> &ready_events);
      void compute_advance_split_mask(VersionInfo &version_info,
                                      UniqueID logical_context_uid,
                                      InnerContext *context,
                                      const FieldMask &version_mask,
                                      std::set<RtEvent> &ready_events,
         const LegionMap<ProjectionEpochID,FieldMask>::aligned &advance_epochs);
      void record_path_only_versions(const FieldMask &version_mask,
                                     const FieldMask &split_mask,
                                     FieldMask &unversioned_mask,
                                     InnerContext *context,
                                     Operation *op, unsigned index,
                                     const RegionUsage &usage,
                                     VersionInfo &version_info,
                                     std::set<RtEvent> &ready_events);
      void advance_versions(FieldMask version_mask, 
                            UniqueID logical_context_uid,
                            InnerContext *physical_context,
                            bool update_parent_state,
                            std::set<RtEvent> &applied_events,
                            bool dedup_opens = false,
                            ProjectionEpochID open_epoch = 0,
                            bool dedup_advances = false, 
                            ProjectionEpochID advance_epoch = 0,
                            const FieldMask *dirty_previous = NULL,
                            const ProjectionInfo *proj_info = NULL,
                            const VersioningSet<> *repl_states_to_use = NULL);
      void reclaim_pending_advance(RtEvent done_event);
      void update_child_versions(InnerContext *context,
                                 LegionColor child_color,
                                 VersioningSet<> &new_states,
                                 std::set<RtEvent> &applied_events);
      void invalidate_version_infos(const FieldMask &invalidate_mask);
      static void filter_version_info(const FieldMask &invalidate_mask,
              LegionMap<VersionID,ManagerVersions>::aligned &to_filter);
    public:
      void print_physical_state(RegionTreeNode *node,
                                const FieldMask &capture_mask,
                                TreeStateLogger *logger);
    public:
      void update_physical_state(PhysicalState *state);
    protected:
      VersionState* create_new_version_state(VersionID vid);
    public:
      RtEvent send_remote_advance(const FieldMask &advance_mask,
                                  bool update_parent_state,
                                  UniqueID logical_context_uid,
                                  bool dedup_opens, 
                                  ProjectionEpochID open_epoch,
                                  bool dedup_advances,
                                  ProjectionEpochID advance_epoch,
                                  const FieldMask *dirty_previous,
                                  const ProjectionInfo *proj_info,
                                  const VersioningSet<> *repl_states_to_user);
      static void handle_remote_advance(Deserializer &derez, Runtime *runtime);
    public:
      RtEvent send_remote_invalidate(AddressSpaceID target,
                                     const FieldMask &invalidate_mask);
      static void handle_remote_invalidate(Deserializer &derez, 
                                           Runtime *runtime);
    public:
      RtEvent send_remote_version_request(FieldMask request_mask,
                                          std::set<RtEvent> &ready_events);
      static void handle_request(Deserializer &derez, Runtime *runtime,
                                 AddressSpaceID source_space);
    public:
      void pack_response(Serializer &rez, AddressSpaceID target,
                         const FieldMask &request_mask);
      static void find_send_infos(
          LegionMap<VersionID,ManagerVersions>::aligned &version_infos,
                                  const FieldMask &request_mask, 
          LegionMap<VersionState*,FieldMask>::aligned& send_infos);
      static void pack_send_infos(Serializer &rez, const
          LegionMap<VersionState*,FieldMask>::aligned& send_infos);
    public:
      void unpack_response(Deserializer &derez, RtUserEvent done_event,
                           const FieldMask &update_mask,
                           std::set<RtEvent> *applied_events);
      static void unpack_send_infos(Deserializer &derez,
          LegionMap<VersionState*,FieldMask>::aligned &infos,
          Runtime *runtime, std::set<RtEvent> &preconditions);
      static void merge_send_infos(
          LegionMap<VersionID,ManagerVersions>::aligned &target_infos,
          const LegionMap<VersionState*,FieldMask>::aligned &source_infos,
                ReferenceMutator *mutator);
      static void handle_response(Deserializer &derez);
    public:
      void find_or_create_unversioned_states(FieldMask unversioned,
          LegionMap<VersionState*,FieldMask>::aligned &unversioned_states,
                                             ReferenceMutator *mutator);
      static void handle_unversioned_request(Deserializer &derez,
                          Runtime *runtime, AddressSpaceID source);
      static void handle_unversioned_response(Deserializer &derez,
                                              Runtime *runtime);
    public:
      static void process_capture_dirty(const void *args);
      static void process_pending_advance(const void *args);
    protected:
      void sanity_check(void);
    public:
      const ContextID ctx;
      RegionTreeNode *const node;
      const unsigned depth;
      Runtime *const runtime;
    protected:
      mutable LocalLock manager_lock;
    protected:
      InnerContext *current_context;
    protected:
      bool is_owner;
      AddressSpaceID owner_space;
    protected: 
      LegionMap<VersionID,ManagerVersions>::aligned current_version_infos;
      LegionMap<VersionID,ManagerVersions>::aligned previous_version_infos;
    protected:
      // On the owner node this is the set of fields for which there are
      // remote copies. On remote nodes this is the set of fields which
      // are locally valid.
      FieldMask remote_valid_fields;
      // Only used on remote nodes to track the set of pending advances
      // which may indicate that remove_valid_fields is stale
      FieldMask pending_remote_advance_summary;
      LegionMap<RtEvent,FieldMask>::aligned pending_remote_advances;
    protected:
      // Owner information about which nodes have remote copies
      LegionMap<AddressSpaceID,FieldMask>::aligned remote_valid;
      // There is something really subtle going on here: note that the
      // both the previous_opens and previous_advances have pairs of
      // UniqueIDs and ProjectionEpochIDs as their keys. This is to 
      // handle the case of virtual mappings, where projection epoch
      // IDs can come from two different logical contexts, but be used
      // in the same physical context due to a virtual mapping. We 
      // disambiguate the projection epoch ID using the context ID.
      // Information about preivous opens
      LegionMap<ProjectionEpoch,FieldMask>::aligned previous_opens;
      // Information about previous advances
      LegionMap<ProjectionEpoch,FieldMask>::aligned previous_advancers;
      // Remote information about outstanding requests we've made
      LegionMap<RtUserEvent,FieldMask>::aligned outstanding_requests;
    };

    typedef DynamicTableAllocator<VersionManager,10,8> VersionManagerAllocator;

    /**
     * \class VersionState
     * This class tracks the physical state information
     * for a particular version number from the persepective
     * of a given logical region.
     */
    class VersionState : public DistributedCollectable,
                         public LegionHeapify<VersionState> {
    public:
      static const AllocationType alloc_type = VERSION_STATE_ALLOC;
    public:
      enum VersionRequestKind {
        CHILD_VERSION_REQUEST,
        INITIAL_VERSION_REQUEST,
        FINAL_VERSION_REQUEST,
      };
    public:
      struct SendVersionStateArgs : public LgTaskArgs<SendVersionStateArgs> {
      public:
        static const LgTaskID TASK_ID = LG_SEND_VERSION_STATE_UPDATE_TASK_ID;
      public:
        VersionState *proxy_this;
        AddressSpaceID target;
        InnerContext *context;
        FieldMask *request_mask;
        VersionRequestKind request_kind;
        RtUserEvent to_trigger;
      };
      struct UpdateStateReduceArgs : public LgTaskArgs<UpdateStateReduceArgs> {
      public:
        static const LgTaskID TASK_ID = LG_UPDATE_VERSION_STATE_REDUCE_TASK_ID;
      public:
        VersionState *proxy_this;
        LegionColor child_color;
        VersioningSet<> *children;
      };
      struct ConvertViewArgs : public LgTaskArgs<ConvertViewArgs> {
      public:
        static const LgTaskID TASK_ID = LG_CONVERT_VIEW_TASK_ID;
      public:
        VersionState *proxy_this;
        PhysicalManager *manager;
        InnerContext *context;
      };
      struct UpdatePendingView : public LgTaskArgs<UpdatePendingView> {
      public:
        static const LgTaskID TASK_ID = LG_UPDATE_PENDING_VIEW_TASK_ID;
      public:
        VersionState *proxy_this;
        LogicalView *view;
        FieldMask *view_mask;
      }; 
      struct RemoveVersionStateRefArgs : 
        public LgTaskArgs<RemoveVersionStateRefArgs> {
      public:
        static const LgTaskID TASK_ID = LG_REMOVE_VERSION_STATE_REF_TASK_ID;
      public:
        VersionState *proxy_this;
        ReferenceSource ref_kind;
      };
      template<VersionRequestKind KIND>
      struct RequestFunctor {
      public:
        RequestFunctor(VersionState *proxy, InnerContext *ctx,
            AddressSpaceID r, const FieldMask &m, std::set<RtEvent> &pre)
          : proxy_this(proxy), context(ctx), requestor(r), 
            mask(m), preconditions(pre) { }
      public:
        void apply(AddressSpaceID target);
      private:
        VersionState *proxy_this;
        InnerContext *context;
        AddressSpaceID requestor;
        const FieldMask &mask;
        std::set<RtEvent> &preconditions;
      };
    public:
      VersionState(VersionID vid, Runtime *rt, DistributedID did,
                   AddressSpaceID owner_space, 
                   RegionTreeNode *node, bool register_now);
      VersionState(const VersionState &rhs);
      virtual ~VersionState(void);
    public:
      VersionState& operator=(const VersionState &rhs);
    public:
      void initialize(ApEvent term_event, const RegionUsage &usage,
                      const FieldMask &user_mask, const InstanceSet &targets,
                      InnerContext *context, unsigned init_index,
                      const std::vector<LogicalView*> &corresponding,
                      std::set<RtEvent> &applied_events);
      void update_path_only_state(PhysicalState *state,
                                  const FieldMask &update_mask) const;
      void update_physical_state(PhysicalState *state, 
                                 const FieldMask &update_mask) const; 
    public: // methods for applying state information
      void merge_physical_state(const PhysicalState *state, 
                                const FieldMask &merge_mask,
                                std::set<RtEvent> &applied_conditions);
      void reduce_open_children(const LegionColor child_color,
                                const FieldMask &update_mask,
                                VersioningSet<> &new_states,
                                std::set<RtEvent> &applied_conditions,
                                bool need_lock, bool local_update);
    public:
      // Must be holding lock from caller when calling these methods
      void insert_materialized_view(MaterializedView *view,
          const FieldMask &view_mask, ReferenceMutator *mutator);
      void insert_reduction_view(ReductionView *view,
          const FieldMask &view_mask, ReferenceMutator *mutator);
      void insert_deferred_view(DeferredView *view,
          const FieldMask &view_mask, ReferenceMutator *mutator);
      void insert_valid_view(LogicalView *view,
          const FieldMask &view_mask, ReferenceMutator *mutator);
      void insert_child_version(VersioningSet<> &child_states, 
          VersionState *state, const FieldMask &state_mask, 
          ReferenceMutator *mutator);
      void remove_child_version(VersioningSet<> &child_states, 
          VersionState *state, ReferenceMutator *mutator);
    public:
      void send_valid_notification(std::set<RtEvent> &applied_events) const;
      void handle_version_state_valid_notification(AddressSpaceID source);
      static void process_version_state_valid_notification(Deserializer &derez,
                                      Runtime *runtime, AddressSpaceID source);
    public:
      virtual void notify_active(ReferenceMutator *mutator);
      virtual void notify_inactive(ReferenceMutator *mutator);
      virtual void notify_remote_inactive(ReferenceMutator *mutator);
      void notify_local_inactive(ReferenceMutator *mutator);
    public:
      virtual void notify_valid(ReferenceMutator *mutator);
      virtual void notify_invalid(ReferenceMutator *mutator);
      virtual void notify_remote_invalid(ReferenceMutator *mutator);
      void notify_local_invalid(ReferenceMutator *mutator);
    public:
      // This method is not currently used, but it is probably
      // not dead code because we're likely going to need it 
      // (or something like it) for optimizing how composite
      // instances fetch only the children they need rather
      // than requesting the full final version state like
      // they currently do
      void request_children_version_state(InnerContext *context,
                                          const FieldMask &request_mask,
                                          std::set<RtEvent> &preconditions);
      void request_initial_version_state(InnerContext *context,
                                         const FieldMask &request_mask,
                                         std::set<RtEvent> &preconditions);
      void request_final_version_state(InnerContext *context,
                                       const FieldMask &request_mask,
                                       std::set<RtEvent> &preconditions);
    public:
      void send_version_state_update(AddressSpaceID target,
                                     InnerContext *context,
                                     const FieldMask &request_mask, 
                                     VersionRequestKind request_kind,
                                     RtUserEvent to_trigger);
      void send_version_state_update_request(AddressSpaceID target, 
                          InnerContext *context, AddressSpaceID src, 
                          RtUserEvent to_trigger, const FieldMask &request_mask,
                          VersionRequestKind request_kind);
      void launch_send_version_state_update(AddressSpaceID target,
                                     InnerContext *context,
                                     RtUserEvent to_trigger, 
                                     const FieldMask &request_mask, 
                                     VersionRequestKind request_kind,
                                     RtEvent precondition=RtEvent::NO_RT_EVENT);
    public:
      void send_version_state(AddressSpaceID source);
      static void handle_version_state_request(Deserializer &derez,
                                      Runtime *runtime, AddressSpaceID source);
      static void handle_version_state_response(Deserializer &derez,
                                      Runtime *runtime, AddressSpaceID source);
    public:
      void handle_version_state_update_request(AddressSpaceID source, 
                                        InnerContext *context,
                                        RtUserEvent to_trigger, 
                                        VersionRequestKind request_kind,
                                        FieldMask &request_mask);
      void handle_version_state_update_response(InnerContext *context,
                                               RtUserEvent to_trigger, 
                                               Deserializer &derez, 
                                               const FieldMask &update, 
                                               VersionRequestKind request_kind);
    public:
      static void process_version_state_reduction(const void *args);
    public:
      void remove_version_state_ref(ReferenceSource ref_kind, 
                                     RtEvent done_event);
      static void process_remove_version_state_ref(const void *args);
    public:
      void convert_view(PhysicalManager *manager, InnerContext *context,
                        ReferenceMutator *mutator);
      static void process_convert_view(const void *args);
      void insert_pending_view(LogicalView *view, const FieldMask &view_mask,
                               ReferenceMutator *mutator);
      static void process_pending_view(const void *args);
    public:
      static void process_version_state_update_request(Runtime *rt, 
                                                Deserializer &derez);
      static void process_version_state_update_response(Runtime *rt,
                                                 Deserializer &derez); 
    public:
      void find_close_fields(FieldMask &test_mask, FieldMask &result_mask);
      void capture_root(CompositeView *target, 
                        const FieldMask &capture_mask,
                        ReferenceMutator *mutator) const;
      void capture(CompositeNode *target, const FieldMask &capture_mask,
                   ReferenceMutator *mutator) const;
      void capture_dirty_instances(const FieldMask &capture_mask, 
                                   VersionState *target) const;
    public:
      const VersionID version_number;
      RegionTreeNode *const logical_node;
    protected:
      mutable LocalLock state_lock;
      // Fields which have been directly written to
      FieldMask dirty_mask;
      // Fields which have reductions
      FieldMask reduction_mask;
      // References to open children that we have
      LegionMap<LegionColor,VersioningSet<> >::aligned open_children;
      // The valid instance views
      LegionMap<LogicalView*, FieldMask,
                VALID_VIEW_ALLOC>::track_aligned valid_views;
      // The valid reduction veiws
      LegionMap<ReductionView*, FieldMask,
                VALID_REDUCTION_ALLOC>::track_aligned reduction_views;
    protected:
      // Fields which we have applied updates to
      FieldMask update_fields;
      // Track when we have valid data for initial and final fields
      LegionMap<RtEvent,FieldMask>::aligned initial_events;
      LegionMap<RtEvent,FieldMask>::aligned final_events;
    protected:
      // Track which nodes we have remote data, note that this only 
      // tracks nodes which have either done a 'merge_physical_state'
      // or 'reduce_open_children' and not nodes that have final 
      // states but haven't contributed any data
      NodeSet remote_valid_instances;
    protected:
      LegionMap<PhysicalManager*,
                std::pair<RtEvent,FieldMask> >::aligned pending_instances;
    protected:
#ifdef DEBUG_LEGION
      // Track our current state 
      bool currently_active;
#endif
      bool currently_valid;
    };

    /**
     * \class RegionTreePath
     * Keep track of the path and states associated with a 
     * given region requirement of an operation.
     */
    class RegionTreePath {
    public:
      RegionTreePath(void);
    public:
      void initialize(unsigned min_depth, unsigned max_depth);
      void register_child(unsigned depth, const LegionColor color);
      void record_aliased_children(unsigned depth, const FieldMask &mask);
      void clear();
    public:
#ifdef DEBUG_LEGION 
      bool has_child(unsigned depth) const;
      LegionColor get_child(unsigned depth) const;
#else
      inline bool has_child(unsigned depth) const
        { return path[depth] != INVALID_COLOR; }
      inline LegionColor get_child(unsigned depth) const
        { return path[depth]; }
#endif
      inline unsigned get_path_length(void) const
        { return ((max_depth-min_depth)+1); }
      inline unsigned get_min_depth(void) const { return min_depth; }
      inline unsigned get_max_depth(void) const { return max_depth; }
    public:
      const FieldMask* get_aliased_children(unsigned depth) const;
    protected:
      std::vector<LegionColor> path;
      LegionMap<unsigned/*depth*/,FieldMask>::aligned interfering_children;
      unsigned min_depth;
      unsigned max_depth;
    };

    /**
     * \class PathTraverser
     * An abstract class which provides the needed
     * functionality for walking a path and visiting
     * all the kinds of nodes along the path.
     */
    class PathTraverser {
    public:
      PathTraverser(RegionTreePath &path);
      PathTraverser(const PathTraverser &rhs);
      virtual ~PathTraverser(void);
    public:
      PathTraverser& operator=(const PathTraverser &rhs);
    public:
      // Return true if the traversal was successful
      // or false if one of the nodes exit stopped early
      bool traverse(RegionTreeNode *start);
    public:
      virtual bool visit_region(RegionNode *node) = 0;
      virtual bool visit_partition(PartitionNode *node) = 0;
    protected:
      RegionTreePath &path;
    protected:
      // Fields are only valid during traversal
      unsigned depth;
      bool has_child;
      LegionColor next_child;
    };

    /**
     * \class NodeTraverser
     * An abstract class which provides the needed
     * functionality for visiting a node in the tree
     * and all of its sub-nodes.
     */
    class NodeTraverser {
    public:
      NodeTraverser(bool force = false)
        : force_instantiation(force) { }
    public:
      virtual bool break_early(void) const { return false; }
      virtual bool visit_only_valid(void) const = 0;
      virtual bool visit_region(RegionNode *node) = 0;
      virtual bool visit_partition(PartitionNode *node) = 0;
    public:
      const bool force_instantiation;
    };

    /**
     * \class LogicalPathRegistrar
     * A class that registers dependences for an operation
     * against all other operation with an overlapping
     * field mask along a given path
     */
    class LogicalPathRegistrar : public PathTraverser {
    public:
      LogicalPathRegistrar(ContextID ctx, Operation *op,
            const FieldMask &field_mask, RegionTreePath &path);
      LogicalPathRegistrar(const LogicalPathRegistrar &rhs);
      virtual ~LogicalPathRegistrar(void);
    public:
      LogicalPathRegistrar& operator=(const LogicalPathRegistrar &rhs);
    public:
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    public:
      const ContextID ctx;
      const FieldMask field_mask;
      Operation *const op;
    };

    /**
     * \class LogicalRegistrar
     * A class that registers dependences for an operation
     * against all other operations with an overlapping
     * field mask.
     */
    class LogicalRegistrar : public NodeTraverser {
    public:
      LogicalRegistrar(ContextID ctx, Operation *op,
                       const FieldMask &field_mask,
                       bool dom);
      LogicalRegistrar(const LogicalRegistrar &rhs);
      ~LogicalRegistrar(void);
    public:
      LogicalRegistrar& operator=(const LogicalRegistrar &rhs);
    public:
      virtual bool visit_only_valid(void) const;
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    public:
      const ContextID ctx;
      const FieldMask field_mask;
      Operation *const op;
      const bool dominate;
    };

    /**
     * \class CurrentInitializer 
     * A class for initializing current states 
     */
    class CurrentInitializer : public NodeTraverser {
    public:
      CurrentInitializer(ContextID ctx);
      CurrentInitializer(const CurrentInitializer &rhs);
      ~CurrentInitializer(void);
    public:
      CurrentInitializer& operator=(const CurrentInitializer &rhs);
    public:
      virtual bool visit_only_valid(void) const;
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    protected:
      const ContextID ctx;
    };

    /**
     * \class CurrentInvalidator 
     * A class for invalidating current states 
     */
    class CurrentInvalidator : public NodeTraverser {
    public:
      CurrentInvalidator(ContextID ctx, bool users_only);
      CurrentInvalidator(const CurrentInvalidator &rhs);
      ~CurrentInvalidator(void);
    public:
      CurrentInvalidator& operator=(const CurrentInvalidator &rhs);
    public:
      virtual bool visit_only_valid(void) const;
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    protected:
      const ContextID ctx;
      const bool users_only;
    };

    /**
     * \class DeletionInvalidator
     * A class for invalidating current states for deletions
     */
    class DeletionInvalidator : public NodeTraverser {
    public:
      DeletionInvalidator(ContextID ctx, const FieldMask &deletion_mask);
      DeletionInvalidator(const DeletionInvalidator &rhs);
      ~DeletionInvalidator(void);
    public:
      DeletionInvalidator& operator=(const DeletionInvalidator &rhs);
    public:
      virtual bool visit_only_valid(void) const;
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    protected:
      const ContextID ctx;
      const FieldMask &deletion_mask;
    };

    /**
     * \class InstanceRef
     * A class for keeping track of references to physical instances
     */
    class InstanceRef : public LegionHeapify<InstanceRef> {
    public:
      InstanceRef(bool composite = false);
      InstanceRef(const InstanceRef &rhs);
      InstanceRef(PhysicalManager *manager, const FieldMask &valid_fields,
                  ApEvent ready_event = ApEvent::NO_AP_EVENT);
      ~InstanceRef(void);
    public:
      InstanceRef& operator=(const InstanceRef &rhs);
    public:
      bool operator==(const InstanceRef &rhs) const;
      bool operator!=(const InstanceRef &rhs) const;
    public:
      inline bool has_ref(void) const { return (manager != NULL); }
      inline ApEvent get_ready_event(void) const { return ready_event; }
      inline void set_ready_event(ApEvent ready) { ready_event = ready; }
      inline PhysicalManager* get_manager(void) const { return manager; }
      inline const FieldMask& get_valid_fields(void) const 
        { return valid_fields; }
      inline void update_fields(const FieldMask &update) 
        { valid_fields |= update; }
    public:
      inline bool is_local(void) const { return local; }
      MappingInstance get_mapping_instance(void) const;
      bool is_virtual_ref(void) const; 
    public:
      // These methods are used by PhysicalRegion::Impl to hold
      // valid references to avoid premature collection
      void add_valid_reference(ReferenceSource source) const;
      void remove_valid_reference(ReferenceSource source) const;
    public:
      Memory get_memory(void) const;
      Reservation get_read_only_reservation(void) const;
    public:
      bool is_field_set(FieldID fid) const;
      LegionRuntime::Accessor::RegionAccessor<
          LegionRuntime::Accessor::AccessorType::Generic>
            get_accessor(void) const;
      LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const;
    public:
      void pack_reference(Serializer &rez) const;
      void unpack_reference(Runtime *rt, Deserializer &derez, RtEvent &ready);
    protected:
      FieldMask valid_fields; 
      ApEvent ready_event;
      PhysicalManager *manager;
      bool local;
    };

    /**
     * \class InstanceSet
     * This class is an abstraction for representing one or more
     * instance references. It is designed to be light-weight and
     * easy to copy by value. It maintains an internal copy-on-write
     * data structure to avoid unnecessary premature copies.
     */
    class InstanceSet {
    public:
      struct CollectableRef : public Collectable, public InstanceRef {
      public:
        CollectableRef(void)
          : Collectable(), InstanceRef() { }
        CollectableRef(const InstanceRef &ref)
          : Collectable(), InstanceRef(ref) { }
        CollectableRef(const CollectableRef &rhs)
          : Collectable(), InstanceRef(rhs) { }
        ~CollectableRef(void) { }
      public:
        CollectableRef& operator=(const CollectableRef &rhs);
      };
      struct InternalSet : public Collectable {
      public:
        InternalSet(size_t size = 0)
          { if (size > 0) vector.resize(size); }
        InternalSet(const InternalSet &rhs) : vector(rhs.vector) { }
        ~InternalSet(void) { }
      public:
        InternalSet& operator=(const InternalSet &rhs)
          { assert(false); return *this; }
      public:
        inline bool empty(void) const { return vector.empty(); }
      public:
        LegionVector<InstanceRef>::aligned vector; 
      };
    public:
      InstanceSet(size_t init_size = 0);
      InstanceSet(const InstanceSet &rhs);
      ~InstanceSet(void);
    public:
      InstanceSet& operator=(const InstanceSet &rhs);
      bool operator==(const InstanceSet &rhs) const;
      bool operator!=(const InstanceSet &rhs) const;
    public:
      InstanceRef& operator[](unsigned idx);
      const InstanceRef& operator[](unsigned idx) const;
    public:
      bool empty(void) const;
      size_t size(void) const;
      void resize(size_t new_size);
      void clear(void);
      void add_instance(const InstanceRef &ref);
      bool is_virtual_mapping(void) const;
    public:
      void pack_references(Serializer &rez) const;
      void unpack_references(Runtime *runtime, Deserializer &derez, 
                             std::set<RtEvent> &ready_events);
    public:
      void add_valid_references(ReferenceSource source) const;
      void remove_valid_references(ReferenceSource source) const;
    public:
      void update_wait_on_events(std::set<ApEvent> &wait_on_events) const;
      void find_read_only_reservations(std::set<Reservation> &locks) const;
    public:
      LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          get_field_accessor(FieldID fid) const;
    protected:
      void make_copy(void);
    protected:
      union {
        CollectableRef* single;
        InternalSet*     multi;
      } refs;
      bool single;
      mutable bool shared;
    };

    /**
     * \class RestrictInfo
     * A class for tracking mapping restrictions based 
     * on region usage.
     */
    class RestrictInfo {
    public:
      struct DeferRestrictedManagerArgs : 
        public LgTaskArgs<DeferRestrictedManagerArgs> {
      public:
        static const LgTaskID TASK_ID = LG_DEFER_RESTRICTED_MANAGER_TASK_ID;
      public:
        PhysicalManager *manager;
      };
    public:
      RestrictInfo(void);
      RestrictInfo(const RestrictInfo &rhs); 
      ~RestrictInfo(void);
    public:
      RestrictInfo& operator=(const RestrictInfo &rhs);
    public:
      inline bool has_restrictions(void) const 
        { return !restrictions.empty(); }
    public:
      void record_restriction(PhysicalManager *inst, const FieldMask &mask);
      void populate_restrict_fields(FieldMask &to_fill) const;
      void clear(void);
      const InstanceSet& get_instances(void);
    public:
      void pack_info(Serializer &rez) const;
      void unpack_info(Deserializer &derez, Runtime *runtime,
                       std::set<RtEvent> &ready_events);
      static void handle_deferred_reference(const void *args);
    protected:
      // We only need garbage collection references on these
      // instances because we know one of two things is always
      // true: either they are attached files so they aren't 
      // subject to memories in which garbage collection will
      // occur, or they are simultaneous restricted, so that
      // enclosing context of the parent task has a valid 
      // reference to them so there is no need for us to 
      // have a valid reference.
      // // Same in Restriction
      LegionMap<PhysicalManager*,FieldMask>::aligned restrictions;
    protected:
      InstanceSet restricted_instances;
    };

    /**
     * \class VersioningInvalidator
     * A class for reseting the versioning managers for 
     * a deleted region (sub)-tree so that version states
     * and the things they point to can be cleaned up
     * by the garbage collector. The better long term
     * answer is to have individual contexts do this.
     */
    class VersioningInvalidator : public NodeTraverser {
    public:
      VersioningInvalidator(void);
      VersioningInvalidator(RegionTreeContext ctx);
    public:
      virtual bool visit_only_valid(void) const { return true; }
      virtual bool visit_region(RegionNode *node);
      virtual bool visit_partition(PartitionNode *node);
    protected:
      const ContextID ctx;
      const bool invalidate_all;
    };

  }; // namespace Internal 
}; // namespace Legion

#endif // __LEGION_ANALYSIS_H__
