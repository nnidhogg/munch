#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TRIE_CHAIN_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TRIE_CHAIN_HPP

#include <span>
#include <string>

#include "munch/dfa/verifier.hpp"

namespace munch::dfa
{
/**
 * @brief Builds the trie chain of a two-mode vocabulary: the verifier of WordPiece segmentation over its initial and
 *        continuation tokens.
 *
 * The scanner takes the longest initial token at the start of the input and then, at each boundary, the longest
 * continuation token, and fails where no token of the mode matches, so an input is segmented when the scan consumes it
 * whole. A run of the scan is a node of its mode's trie, the initial tokens' or the continuation tokens', begun at a
 * boundary: the unarmed run begun at the last boundary, and the armed runs begun at earlier boundaries that are still
 * alive, each a proper prefix of a longer token of its mode. A state is the oldest armed run's node, which spells the
 * unresolved tail, with the depth inside the tail at which the unarmed run began; the armed runs between are not
 * stored, since every boundary inside the tail is forced by longest match, the first at the longest token of the tail's
 * mode that is a prefix of the tail, the next at the longest continuation token from there, and so on, which is the
 * scanner's own reading of the tail. That reading is done once per trie node at construction, the node's chain, and the
 * armed runs of every state with the node as its tail are read off the chain. A step reads the byte into every run, a
 * run with no child dropping, and is refused when an armed run reaches a token or the unarmed run drops; a boundary
 * requires the unarmed run to be at a token, arms it when a longer token extends it, and begins a fresh unarmed run at
 * the continuation trie's root. A state accepts when its unarmed run is at a token; the start, nothing read, accepts
 * the empty input.
 *
 * Built by exploration from the start over every byte, each state interned as it is found, so that the states are the
 * reachable ones only; the verifier's constructor then trims the result by coaccessibility. With N the nodes of both
 * tries, P the nodes other than the root with a child and c the longest chain, which is at most the longest token, the
 * states are at most 1 + (N - 1) + P * c: the start, each node as the unarmed run with nothing armed, and each pending
 * tail with the unarmed run begun at a boundary of its chain. With L the longest token, the chains cost O(P c L) time
 * and O(P c) memory, so O(P L^2) and O(P L), since each pending node's chain walks the rest of the tail from each of
 * its at most c boundaries and is stored as one link per boundary. The verifier accepts at most one marking per byte
 * string, the scanner's segmentation of it, so its domain is the byte strings the scanner consumes whole; a byte no
 * token names is in no domain string. A token may be in both sets, and a set may be empty: with no initial token the
 * domain is the empty input alone, with no continuation token it is the initial tokens. An empty token is refused.
 * @param initial The initial tokens, the tokens of the first segment.
 * @param continuation The continuation tokens, the tokens of every later segment.
 * @return The trie-chain verifier.
 * @throws std::invalid_argument If a token is empty; the message names the set and the token's position in it.
 */
[[nodiscard]] Verifier trie_chain(std::span<const std::string> initial, std::span<const std::string> continuation);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TRIE_CHAIN_HPP
