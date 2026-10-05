#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_BUILDER_DBG_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_BUILDER_DBG_HPP

#include "munch/core/builder.hpp"

/**
 * @brief A Builder whose compiled automaton a probe reads, Builder_dbg.
 */
namespace munch::tools::probes
{
/**
 * @brief A Builder exposing its compiled automaton.
 */
class Builder_dbg : public core::Builder
{
public:
    /**
     * @brief The Builder's compiled automaton, made public.
     */
    using Builder::dfa;
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_BUILDER_DBG_HPP
