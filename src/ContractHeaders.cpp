/**
 * @file ContractHeaders.cpp
 * @brief Compile-firewall anchor: includes every public header in one TU
 *        compiled into the library, so the frozen header set can never
 *        drift apart silently.
 */

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/BlockStoreTooling.hpp>
#include <sgns-blockstore/Capabilities.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/FaultController.hpp>
#include <sgns-blockstore/Options.hpp>
