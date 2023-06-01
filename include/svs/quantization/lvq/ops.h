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

// svs
#include "svs/concepts/data.h"
#include "svs/concepts/distance.h"
#include "svs/core/medioid.h"
#include "svs/lib/misc.h"
#include "svs/lib/range.h"
#include "svs/lib/threads.h"
#include "svs/quantization/lvq/vectors.h"

// stl
#include <string>

//
// File Summary:
//
// Often, it is helpful to perform some operation on each vector element in a dataset before
// quantizing and often this operation is dependent on some global property of the dataset.
//
// For example, one may wish to remove the remove the mean value of each dimension of a
// dataset and apply some kind of distribution dependent scaling parameter.
//
// Furthermore, computation of these parameters may not be as straight forward at billion
// scale, where simple implementations of algorithms (such as component-wise means) may
// be susceptible to floating point errors.
//
// Finally, the performed operation (e.g., mean removal) may require modifications in the
// distance function in order to return the correct computation.
//
// This file collects these pre-ops into a single location.
//
// In general, a constructed pre-operation is a functor that implements:
// ```
// template<typename Distance, data::ImmutableMemoryDataset Data, threads::ThreadPool Pool>
// std::tuple<...> operator()(const Distance&, const Data&, Pool&);
// ```
// where the returned `tuple` contains 3 elements:
//
// (1) A potentially modified distance function to reverse the element-wise operation.
// (2) A functor `f` to be applied to each element of the dataset.
// (3) Any miscellaneous data that could be helpful to the caller (implementation
//     dependent). See the particular pre-op documentation for what is returned.
//
// The returned functor `f` will have the following properties:
// * Must be copy-constructed by threads to construct an independent per-thread functor.
// * Can be applied independently to each element in the dataset.
// * Upon application, does not modify its corresponding dataset entry.
//

namespace svs::quantization::lvq {

///
/// Dataset pre-processing routines.
///
/// Expected API:
/// struct /*impl*/ : DatasetPreOpBase {
///     // Return a string identifier for this per-op.
///     static std::string name();
///
///     // Return a potentially modified distance type to use with a modified dataset.
///     //
///     // -- Rational: Operations that are used to modify the dataset (such as removing
///     // a global bias) may want to return a slightly different distance function that
///     // is capable of modifying the query in a more efficient distance computation.
///     //
///     // This type alias provides a way of querying what the modified distance type
///     // will be.
///     template<typename Distance> using distance_type = /*impl*/;
///
///     // Misc auxiliary return type.
///     using misc_type = /*impl*/;
///
///     // Perform the given pre-op on the dataset, returning a tuple consisting of:
///     // (1) A potentially modified distance type to be used on a modified dataset.
///     // (2) A map functor that may be applied to each element in the dataset. The
///     //     modified distance function should preserve distances under the modified
///     //     dataset.
///     template<
///         typename Distance,
///         data::ImmutableMemoryDataset Data,
///         svs::threads::ThreadPool Pool
///         >
///     std::tuple<distance_type<Distance>, /*impl*/, misc_type> operator()(
///         const Distance&, const Data&, Pool&
///     );
/// };
///
struct DatasetPreOpBase {};

template <typename T>
concept DatasetPreOp = requires { std::derived_from<T, DatasetPreOpBase>; };

///
/// No Operation.
///
struct Noop : public DatasetPreOpBase {
    static std::string name() { return "preop-none"; }

    // Distance types are not modified.
    template <typename Distance> using distance_type = Distance;

    // The element-wise mapping function.
    using map_type = lib::identity;
    using misc_type = lib::Empty;

    // Don't modify the distance type and return the identity map.
    template <
        typename Distance,
        data::ImmutableMemoryDataset Data,
        svs::threads::ThreadPool Pool>
    std::tuple<distance_type<Distance>, map_type, misc_type>
    operator()(const Distance& distance, Data& /*unused*/, Pool& /*unused*/) const {
        return std::make_tuple(distance, map_type(), lib::Empty());
    }
};

///
/// Element-wise functor that performs the operation:
/// ```
/// scale[i] * (x[i] + shift[i])
/// ```
/// For each component `i` in a vector dataset.
///
template <typename T> class ScaleShift {
  public:
    using vector_type = std::vector<double>;
    using ptr_type = std::shared_ptr<vector_type>;

    ScaleShift(const ptr_type& scale, const ptr_type& shift)
        : scale_{scale}
        , shift_{shift}
        , modified_buffer_(scale->size()) {
        if ((*scale_).size() != (*shift_).size()) {
            throw ANNEXCEPTION("Scale and shift mismatch!");
        }
    }

    ScaleShift(const vector_type& scale, const vector_type& shift)
        : ScaleShift{
              std::make_shared<vector_type>(scale), std::make_shared<vector_type>(shift)} {}

    ///
    /// Return the vector dimensionality that this operator is constructed to operate on.
    ///
    size_t size() const { return scale_->size(); }

    ///
    /// Subtract the stored bias from the data span and return a data-span to the modified
    /// data. Does not modify its argument.
    ///
    /// The returned span is valid so long as:
    /// * The parent `RemoveBias` is live.
    /// * `operator()` is not called on another data.
    ///
    /// Pre-conditions:
    /// * `data.size() == size()`
    ///
    std::span<const T> operator()(const std::span<const T>& data) {
        assert(data.size() == size());
        assert(shift_->size() == scale_->size());
        assert(modified_buffer_.size() == size());

        const auto& shift = *shift_;
        const auto& scale = *scale_;
        for (size_t i = 0, imax = size(); i < imax; ++i) {
            modified_buffer_[i] =
                static_cast<T>(scale[i] * (static_cast<double>(data[i]) + shift[i]));
        }
        return lib::as_const_span(modified_buffer_);
    }

  private:
    // Generally, don't modify `bias_` because it's meant to be shared among thread-local
    // copies.
    std::shared_ptr<std::vector<double>> scale_;
    std::shared_ptr<std::vector<double>> shift_;
    std::vector<T> modified_buffer_;
};

// Determine the average value for each component.
// Remove this bias from each component and return a distance functor that is able to
// lazily re-apply the bias.
struct VectorBias : public DatasetPreOpBase {
    static std::string name() { return "preop-vector-bias"; }

    template <typename T> using element_type_t = typename T::element_type;

    // After applying the bias-removal functor, the bias needs to be re-applied at distance
    // computation time.
    //
    // To make this more efficient, special distance functions can be used.
    template <typename Distance> using distance_type = biased_distance_t<Distance>;
    using misc_type = std::vector<double>;

    ///
    /// Compute the mean of each dimension in the dataset.
    /// Return a tuple consisting of:
    /// (1) A distance function that preserves the semantics of the original distance
    ///     function but can be applied to a dataset that has had the per-dimension mean
    ///     removed from each element.
    /// (2) A copyable map operator that operates subtracts the componentwise mean from
    ///     an entry in the dataset.
    ///
    template <
        typename Distance,
        data::ImmutableMemoryDataset Data,
        svs::threads::ThreadPool Pool>
    std::tuple<distance_type<Distance>, ScaleShift<element_type_t<Data>>, misc_type>
    operator()(const Distance& /*unused*/, const Data& data, Pool& pool) const {
        using T = element_type_t<Data>;
        using NewDistance = distance_type<Distance>;

        // Compute the component-wise mean of the dataset.
        // Negate the medioid to get the bias we've applied to the dataset.
        std::vector<double> means_f64 = utils::compute_medioid(data, pool);
        std::vector<float> means_f32{means_f64.begin(), means_f64.end()};

        range::negate(means_f64);
        auto ones = std::vector<double>(means_f64.size(), 1.0);
        return std::make_tuple(
            NewDistance{means_f32}, ScaleShift<T>(ones, means_f64), means_f64
        );
    }
};

///
/// `BiasVariance` pre-operation that scales the per-dimension standard deviation by
/// `scale`. Let `X` be a dataset, `x[j]` be the `j`th element of `X` and `x[j][i]` be the
/// `i`th component of `x[j]`.
///
/// This pre-operation computes `mean` and `stdev` where
/// ```
/// mean[i] = sum(x[j][i], j in eachindex(X))
/// ```
/// (i.e., the mean of the `i`th components) and
/// ```
/// stdev[i] = stdev(x[j][i], j in eachindex(X))
/// ```
/// (i.e., the standard deviation of the `i`th components).
///
/// The standard deviation is then scaled by `scale`.
///
/// Returns a map operator that subtracts out the mean and divides by the scaled standard
/// deviation.
///
class BiasVariance : public DatasetPreOpBase {
  private:
    // Members
    float scale_;

  public:
    // Constructor
    BiasVariance(float scale)
        : scale_{scale} {}

    // Methods
    static std::string name() { return "preop-variance-bias"; }
    const float& scale() const { return scale_; }

    template <typename T> using element_type_t = typename T::element_type;
    template <typename Distance> using distance_type = Distance;

    // Miscellaneous return type.
    //
    // Returns the removed means and the scaled standard deviations.
    struct MeanScaledStdev {
        MeanScaledStdev(std::vector<double> mean, std::vector<double> stdev)
            : mean{std::move(mean)}
            , stdev{std::move(stdev)} {}

        std::vector<double> mean;
        std::vector<double> stdev;
    };
    using misc_type = MeanScaledStdev;

    template <
        typename Distance,
        data::ImmutableMemoryDataset Data,
        svs::threads::ThreadPool Pool>
    std::tuple<distance_type<Distance>, ScaleShift<element_type_t<Data>>, misc_type>
    operator()(const Distance& distance, const Data& data, Pool& pool) const {
        using T = element_type_t<Data>;

        std::vector<double> means = utils::compute_medioid(data, pool);
        std::vector<double> variance =
            utils::op_pairwise(data, utils::CountVariance(means), pool);

        // Compute the component-wise standard deviation.
        auto stdev = std::vector<double>(variance.size());
        range::sqrt(variance, stdev);

        // Scale the standard deviation.
        range::transform(stdev, range::MulBy(scale()));

        // Construct the miscellaneous return type before preparing the mean and
        // standard deviation for the map operator.
        auto misc = MeanScaledStdev(means, stdev);

        // Negate the bias and invert the bias.
        range::negate(means);
        range::invert(stdev);
        return std::make_tuple(distance, ScaleShift<T>(stdev, means), misc);
    }
};

} // namespace svs::quantization::lvq