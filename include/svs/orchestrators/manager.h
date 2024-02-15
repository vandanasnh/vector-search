/**
 *    Copyright (C) 2023-present, Intel Corporation
 *
 *    You can redistribute and/or modify this software under the terms of the
 *    GNU Affero General Public License version 3.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    version 3 along with this software. If not, see
 *    <https://www.gnu.org/licenses/agpl-3.0.en.html>.
 */

#pragma once

#include "svs/core/data/simple.h"
#include "svs/core/distance.h"
#include "svs/core/query_result.h"
#include "svs/lib/datatype.h"
#include "svs/lib/threads/threadpool.h"

#include <concepts>
#include <memory>
#include <numeric>
#include <type_traits>

namespace svs::manager {

///
/// Top level Manager
///

class ManagerInterface {
  public:
    ManagerInterface() = default;

    virtual void
    search(AnonymousArray<2> data, size_t nneighbors, QueryResultView<size_t> result) = 0;

    // Data Interface
    virtual size_t size() const = 0;
    virtual size_t dimensions() const = 0;

    // Threading interface
    virtual bool can_change_threads() const = 0;
    virtual size_t get_num_threads() const = 0;
    virtual void set_num_threads(size_t) = 0;

    // Delete the special member functions.
    ManagerInterface(const ManagerInterface&) = delete;
    ManagerInterface& operator=(const ManagerInterface&) = delete;
    ManagerInterface(ManagerInterface&&) = delete;
    ManagerInterface& operator=(ManagerInterface&&) = delete;

    virtual ~ManagerInterface() = default;
};

///
/// The base implementation for types meant to implement polymorphic Manager interface.
/// The goal of this type is to wrap a concrete implementation of type `T` with the
///
template <typename QueryType, typename Impl, std::derived_from<ManagerInterface> IFace>
class ManagerImpl : public IFace {
  public:
    explicit ManagerImpl(Impl implementation)
        : IFace{}
        , implementation_{std::move(implementation)} {}

    ///
    /// Construct a `ManagerImpl` piecewise by calling it's implementation's constructor
    /// directly.
    ///
    template <typename... Args>
    explicit ManagerImpl(Args&&... args)
        : IFace{}
        , implementation_{std::forward<Args>(args)...} {}

    // Search Interface
    void search(AnonymousArray<2> data, size_t nneighbors, QueryResultView<size_t> result)
        override {
        // TODO (Mark) For now - only allow implementations to support a single query
        // type.
        //
        // Generalizing this to multiple query types will require some metaprogramming
        // dances.
        if (data.type() == datatype_v<QueryType>) {
            const auto view = data::ConstSimpleDataView<QueryType>(data);
            implementation_.search(view, nneighbors, result);
        } else {
            throw ANNEXCEPTION(
                "Unsupported datatype! Got: {}. Expected: {}.",
                data.type(),
                (datatype_v<QueryType>)
            );
        }
    }

    // Data Interface
    size_t size() const override { return implementation_.size(); }
    size_t dimensions() const override { return implementation_.dimensions(); }

    // Threading interface.
    bool can_change_threads() const override {
        return implementation_.can_change_threads();
    }
    size_t get_num_threads() const override { return implementation_.get_num_threads(); }
    void set_num_threads(size_t num_threads) override {
        implementation_.set_num_threads(num_threads);
    }

  protected:
    Impl& impl() { return implementation_; }
    const Impl& impl() const { return implementation_; }

  private:
    Impl implementation_;
};

///
/// @brief Do I need to document this also?
///
template <std::derived_from<ManagerInterface> IFace> class IndexManager {
  public:
    template <typename Impl>
        requires std::is_base_of_v<IFace, Impl>
    explicit IndexManager(std::unique_ptr<Impl> impl)
        : impl_{std::move(impl)} {}

    ///
    /// @brief Perform a batch search over the provided queries.
    ///
    /// @tparam QueryType The data type used for each component of the vector elements
    ///     of the query data structure.
    ///
    /// @param queries The batch of queries to find neighbors for.
    /// @param nneighbors The number of (potentially approximate) neighbors to return for
    ///     each query.
    ///
    /// @returns A `QueryResult` containing the `nneighbors` nearest neighbors for each
    ///     query and the computed distances.
    ///     Row `i` in the result corresponds to the neighbors for the `i`th query.
    ///     Neighbors within each row are ordered from nearest to furthest.
    ///
    /// The backend-implementations may not support all templated query types.
    /// If the implementation does not support the given query type, throws `ANNException`.
    ///
    template <typename QueryType>
    QueryResult<size_t>
    search(data::ConstSimpleDataView<QueryType> queries, size_t nneighbors) {
        QueryResult<size_t> result{queries.size(), nneighbors};
        search(queries, nneighbors, result.view());
        return result;
    }

    /// @copydoc search(data::ConstSimpleDataView<QueryType>,size_t)
    template <data::ImmutableMemoryDataset Data>
    QueryResult<size_t> search(const Data& queries, size_t nneighbors) {
        return search(queries.cview(), nneighbors);
    }

    template <typename QueryType>
    void search(
        data::ConstSimpleDataView<QueryType> queries,
        size_t nneighbors,
        QueryResultView<size_t> result
    ) {
        impl_->search(AnonymousArray<2>(queries), nneighbors, result);
    }

    ///// Data Interface

    /// @brief Return the number of elements in the indexed dataset.
    size_t size() const { return impl_->size(); }

    /// @brief Return the logical number of dimensions of each vector in the indexed
    /// dataset.
    size_t dimensions() const { return impl_->dimensions(); }

    ///// Threading Interface

    ///
    /// @brief Return whether the back-end implementation can change the number of threads.
    ///
    bool can_change_threads() const { return impl_->can_change_threads(); }

    ///
    /// @brief Return the current number of worker threads used by this index for searches.
    ///
    size_t get_num_threads() const { return impl_->get_num_threads(); }

    ///
    /// @brief Set the number of threads to use for searching.
    ///
    /// @param num_threads The number of threads to use. If set to ``0``, will implicitly
    ///     default to ``1``.
    ///
    /// Only effective if ``can_change_threads()`` returns ``true``.
    ///
    void set_num_threads(size_t num_threads) {
        impl_->set_num_threads(std::max(size_t(1), num_threads));
    }

    // The implementation is `protected` instead of private because derived classes
    // should extent the interface provided by the base Manager.
    //
    // TODO: This could be exposed via a `get_impl()` accessor - but is that really
    // any different than just grabbing the interface pointer directly?
  protected:
    std::unique_ptr<IFace> impl_;
};

} // namespace svs::manager
