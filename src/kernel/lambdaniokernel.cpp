// Copyright (c) 2022-present The Lambdanio Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define LAMBDANIOKERNEL_BUILD

#include <kernel/lambdaniokernel.h>

#include <chain.h>
#include <coins.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/chainparams.h>
#include <kernel/checks.h>
#include <kernel/context.h>
#include <kernel/notifications_interface.h>
#include <kernel/warning.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/task_runner.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using kernel::ChainstateRole;
using util::ImmediateTaskRunner;

// Define G_TRANSLATION_FUN symbol in liblambdaniokernel library so users of the
// library aren't required to export this symbol
extern const TranslateFn G_TRANSLATION_FUN{nullptr};

static const kernel::Context ldok_context_static{};

namespace {

bool is_valid_flag_combination(script_verify_flags flags)
{
    if (flags & SCRIPT_VERIFY_CLEANSTACK && ~flags & (SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS)) return false;
    if (flags & SCRIPT_VERIFY_WITNESS && ~flags & SCRIPT_VERIFY_P2SH) return false;
    return true;
}

class WriterStream
{
private:
    ldok_WriteBytes m_writer;
    void* m_user_data;

public:
    WriterStream(ldok_WriteBytes writer, void* user_data)
        : m_writer{writer}, m_user_data{user_data} {}

    //
    // Stream subset
    //
    void write(std::span<const std::byte> src)
    {
        if (m_writer(src.data(), src.size(), m_user_data) != 0) {
            throw std::runtime_error("Failed to write serialization data");
        }
    }

    template <typename T>
    WriterStream& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return *this;
    }
};

template <typename C, typename CPP>
struct Handle {
    static C* ref(CPP* cpp_type)
    {
        return reinterpret_cast<C*>(cpp_type);
    }

    static const C* ref(const CPP* cpp_type)
    {
        return reinterpret_cast<const C*>(cpp_type);
    }

    template <typename... Args>
    static C* create(Args&&... args)
    {
        auto cpp_obj{std::make_unique<CPP>(std::forward<Args>(args)...)};
        return ref(cpp_obj.release());
    }

    static C* copy(const C* ptr)
    {
        auto cpp_obj{std::make_unique<CPP>(get(ptr))};
        return ref(cpp_obj.release());
    }

    static const CPP& get(const C* ptr)
    {
        return *reinterpret_cast<const CPP*>(ptr);
    }

    static CPP& get(C* ptr)
    {
        return *reinterpret_cast<CPP*>(ptr);
    }

    static void operator delete(void* ptr)
    {
        delete reinterpret_cast<CPP*>(ptr);
    }
};

} // namespace

struct ldok_BlockTreeEntry: Handle<ldok_BlockTreeEntry, CBlockIndex> {};
struct ldok_Block : Handle<ldok_Block, std::shared_ptr<const CBlock>> {};
struct ldok_BlockValidationState : Handle<ldok_BlockValidationState, BlockValidationState> {};

namespace {

BCLog::Level get_bclog_level(ldok_LogLevel level)
{
    switch (level) {
    case ldok_LogLevel_INFO: {
        return BCLog::Level::Info;
    }
    case ldok_LogLevel_DEBUG: {
        return BCLog::Level::Debug;
    }
    case ldok_LogLevel_TRACE: {
        return BCLog::Level::Trace;
    }
    }
    assert(false);
}

BCLog::LogFlags get_bclog_flag(ldok_LogCategory category)
{
    switch (category) {
    case ldok_LogCategory_BENCH: {
        return BCLog::LogFlags::BENCH;
    }
    case ldok_LogCategory_BLOCKSTORAGE: {
        return BCLog::LogFlags::BLOCKSTORAGE;
    }
    case ldok_LogCategory_COINDB: {
        return BCLog::LogFlags::COINDB;
    }
    case ldok_LogCategory_LEVELDB: {
        return BCLog::LogFlags::LEVELDB;
    }
    case ldok_LogCategory_MEMPOOL: {
        return BCLog::LogFlags::MEMPOOL;
    }
    case ldok_LogCategory_PRUNE: {
        return BCLog::LogFlags::PRUNE;
    }
    case ldok_LogCategory_RAND: {
        return BCLog::LogFlags::RAND;
    }
    case ldok_LogCategory_REINDEX: {
        return BCLog::LogFlags::REINDEX;
    }
    case ldok_LogCategory_VALIDATION: {
        return BCLog::LogFlags::VALIDATION;
    }
    case ldok_LogCategory_KERNEL: {
        return BCLog::LogFlags::KERNEL;
    }
    case ldok_LogCategory_ALL: {
        return BCLog::LogFlags::ALL;
    }
    }
    assert(false);
}

ldok_SynchronizationState cast_state(SynchronizationState state)
{
    switch (state) {
    case SynchronizationState::INIT_REINDEX:
        return ldok_SynchronizationState_INIT_REINDEX;
    case SynchronizationState::INIT_DOWNLOAD:
        return ldok_SynchronizationState_INIT_DOWNLOAD;
    case SynchronizationState::POST_INIT:
        return ldok_SynchronizationState_POST_INIT;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

ldok_Warning cast_ldok_warning(kernel::Warning warning)
{
    switch (warning) {
    case kernel::Warning::UNKNOWN_NEW_RULES_ACTIVATED:
        return ldok_Warning_UNKNOWN_NEW_RULES_ACTIVATED;
    case kernel::Warning::LARGE_WORK_INVALID_CHAIN:
        return ldok_Warning_LARGE_WORK_INVALID_CHAIN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

struct LoggingConnection {
    std::unique_ptr<std::list<std::function<void(const std::string&)>>::iterator> m_connection;
    void* m_user_data;
    std::function<void(void* user_data)> m_deleter;

    LoggingConnection(ldok_LogCallback callback, void* user_data, ldok_DestroyCallback user_data_destroy_callback)
    {
        LOCK(cs_main);

        auto connection{LogInstance().PushBackCallback([callback, user_data](const std::string& str) { callback(user_data, str.c_str(), str.length()); })};

        // Only start logging if we just added the connection.
        if (LogInstance().NumConnections() == 1 && !LogInstance().StartLogging()) {
            LogError("Logger start failed.");
            LogInstance().DeleteCallback(connection);
            if (user_data && user_data_destroy_callback) {
                user_data_destroy_callback(user_data);
            }
            throw std::runtime_error("Failed to start logging");
        }

        m_connection = std::make_unique<std::list<std::function<void(const std::string&)>>::iterator>(connection);
        m_user_data = user_data;
        m_deleter = user_data_destroy_callback;

        LogDebug(BCLog::KERNEL, "Logger connected.");
    }

    ~LoggingConnection()
    {
        LOCK(cs_main);
        LogDebug(BCLog::KERNEL, "Logger disconnecting.");

        // Switch back to buffering by calling DisconnectTestLogger if the
        // connection that we are about to remove is the last one.
        if (LogInstance().NumConnections() == 1) {
            LogInstance().DisconnectTestLogger();
        } else {
            LogInstance().DeleteCallback(*m_connection);
        }

        m_connection.reset();
        if (m_user_data && m_deleter) {
            m_deleter(m_user_data);
        }
    }
};

class KernelNotifications final : public kernel::Notifications
{
private:
    ldok_NotificationInterfaceCallbacks m_cbs;

public:
    KernelNotifications(ldok_NotificationInterfaceCallbacks cbs)
        : m_cbs{cbs}
    {
    }

    ~KernelNotifications()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data_destroy = nullptr;
        m_cbs.user_data = nullptr;
    }

    kernel::InterruptResult blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress) override
    {
        if (m_cbs.block_tip) m_cbs.block_tip(m_cbs.user_data, cast_state(state), ldok_BlockTreeEntry::ref(&index), verification_progress);
        return {};
    }
    void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override
    {
        if (m_cbs.header_tip) m_cbs.header_tip(m_cbs.user_data, cast_state(state), height, timestamp, presync ? 1 : 0);
    }
    void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override
    {
        if (m_cbs.progress) m_cbs.progress(m_cbs.user_data, title.original.c_str(), title.original.length(), progress_percent, resume_possible ? 1 : 0);
    }
    void warningSet(kernel::Warning id, const bilingual_str& message) override
    {
        if (m_cbs.warning_set) m_cbs.warning_set(m_cbs.user_data, cast_ldok_warning(id), message.original.c_str(), message.original.length());
    }
    void warningUnset(kernel::Warning id) override
    {
        if (m_cbs.warning_unset) m_cbs.warning_unset(m_cbs.user_data, cast_ldok_warning(id));
    }
    void flushError(const bilingual_str& message) override
    {
        if (m_cbs.flush_error) m_cbs.flush_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
    void fatalError(const bilingual_str& message) override
    {
        if (m_cbs.fatal_error) m_cbs.fatal_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
};

class KernelValidationInterface final : public CValidationInterface
{
public:
    ldok_ValidationInterfaceCallbacks m_cbs;

    explicit KernelValidationInterface(const ldok_ValidationInterfaceCallbacks vi_cbs) : m_cbs{vi_cbs} {}

    ~KernelValidationInterface()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data = nullptr;
        m_cbs.user_data_destroy = nullptr;
    }

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& stateIn) override
    {
        if (m_cbs.block_checked) {
            m_cbs.block_checked(m_cbs.user_data,
                                ldok_Block::copy(ldok_Block::ref(&block)),
                                ldok_BlockValidationState::ref(&stateIn));
        }
    }

    void NewPoWValidBlock(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block) override
    {
        if (m_cbs.pow_valid_block) {
            m_cbs.pow_valid_block(m_cbs.user_data,
                                  ldok_Block::copy(ldok_Block::ref(&block)),
                                  ldok_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_connected) {
            m_cbs.block_connected(m_cbs.user_data,
                                  ldok_Block::copy(ldok_Block::ref(&block)),
                                  ldok_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_disconnected) {
            m_cbs.block_disconnected(m_cbs.user_data,
                                     ldok_Block::copy(ldok_Block::ref(&block)),
                                     ldok_BlockTreeEntry::ref(pindex));
        }
    }
};

struct ContextOptions {
    mutable Mutex m_mutex;
    std::unique_ptr<const CChainParams> m_chainparams GUARDED_BY(m_mutex);
    std::shared_ptr<KernelNotifications> m_notifications GUARDED_BY(m_mutex);
    std::shared_ptr<KernelValidationInterface> m_validation_interface GUARDED_BY(m_mutex);
};

class Context
{
public:
    std::unique_ptr<kernel::Context> m_context;

    std::shared_ptr<KernelNotifications> m_notifications;

    std::unique_ptr<util::SignalInterrupt> m_interrupt;

    std::unique_ptr<ValidationSignals> m_signals;

    std::unique_ptr<const CChainParams> m_chainparams;

    std::shared_ptr<KernelValidationInterface> m_validation_interface;

    Context(const ContextOptions* options, bool& sane)
        : m_context{std::make_unique<kernel::Context>()},
          m_interrupt{std::make_unique<util::SignalInterrupt>()}
    {
        if (options) {
            LOCK(options->m_mutex);
            if (options->m_chainparams) {
                m_chainparams = std::make_unique<const CChainParams>(*options->m_chainparams);
            }
            if (options->m_notifications) {
                m_notifications = options->m_notifications;
            }
            if (options->m_validation_interface) {
                m_signals = std::make_unique<ValidationSignals>(std::make_unique<ImmediateTaskRunner>());
                m_validation_interface = options->m_validation_interface;
                m_signals->RegisterSharedValidationInterface(m_validation_interface);
            }
        }

        if (!m_chainparams) {
            m_chainparams = CChainParams::Main();
        }
        if (!m_notifications) {
            m_notifications = std::make_shared<KernelNotifications>(ldok_NotificationInterfaceCallbacks{
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
        }

        if (!kernel::SanityChecks(*m_context)) {
            sane = false;
        }
    }

    ~Context()
    {
        if (m_signals) {
            m_signals->UnregisterSharedValidationInterface(m_validation_interface);
        }
    }
};

//! Helper struct to wrap the ChainstateManager-related Options
struct ChainstateManagerOptions {
    mutable Mutex m_mutex;
    ChainstateManager::Options m_chainman_options GUARDED_BY(m_mutex);
    node::BlockManager::Options m_blockman_options GUARDED_BY(m_mutex);
    std::shared_ptr<const Context> m_context;
    node::ChainstateLoadOptions m_chainstate_load_options GUARDED_BY(m_mutex);

    ChainstateManagerOptions(const std::shared_ptr<const Context>& context, const fs::path& data_dir, const fs::path& blocks_dir)
        : m_chainman_options{ChainstateManager::Options{
              .chainparams = *context->m_chainparams,
              .datadir = data_dir,
              .notifications = *context->m_notifications,
              .signals = context->m_signals.get()}},
          m_blockman_options{node::BlockManager::Options{
              .chainparams = *context->m_chainparams,
              .blocks_dir = blocks_dir,
              .notifications = *context->m_notifications,
              .block_tree_db_params = DBParams{
                  .path = data_dir / "blocks" / "index",
                  .cache_bytes = kernel::CacheSizes{DEFAULT_KERNEL_CACHE}.block_tree_db,
              }}},
          m_context{context}, m_chainstate_load_options{node::ChainstateLoadOptions{}}
    {
    }
};

struct ChainMan {
    std::unique_ptr<ChainstateManager> m_chainman;
    std::shared_ptr<const Context> m_context;

    ChainMan(std::unique_ptr<ChainstateManager> chainman, std::shared_ptr<const Context> context)
        : m_chainman(std::move(chainman)), m_context(std::move(context)) {}
};

} // namespace

struct ldok_Transaction : Handle<ldok_Transaction, std::shared_ptr<const CTransaction>> {};
struct ldok_TransactionOutput : Handle<ldok_TransactionOutput, CTxOut> {};
struct ldok_ScriptPubkey : Handle<ldok_ScriptPubkey, CScript> {};
struct ldok_LoggingConnection : Handle<ldok_LoggingConnection, LoggingConnection> {};
struct ldok_ContextOptions : Handle<ldok_ContextOptions, ContextOptions> {};
struct ldok_Context : Handle<ldok_Context, std::shared_ptr<const Context>> {};
struct ldok_ChainParameters : Handle<ldok_ChainParameters, CChainParams> {};
struct ldok_ChainstateManagerOptions : Handle<ldok_ChainstateManagerOptions, ChainstateManagerOptions> {};
struct ldok_ChainstateManager : Handle<ldok_ChainstateManager, ChainMan> {};
struct ldok_Chain : Handle<ldok_Chain, CChain> {};
struct ldok_BlockSpentOutputs : Handle<ldok_BlockSpentOutputs, std::shared_ptr<CBlockUndo>> {};
struct ldok_TransactionSpentOutputs : Handle<ldok_TransactionSpentOutputs, CTxUndo> {};
struct ldok_Coin : Handle<ldok_Coin, Coin> {};
struct ldok_BlockHash : Handle<ldok_BlockHash, uint256> {};
struct ldok_TransactionInput : Handle<ldok_TransactionInput, CTxIn> {};
struct ldok_TransactionOutPoint: Handle<ldok_TransactionOutPoint, COutPoint> {};
struct ldok_Txid: Handle<ldok_Txid, Txid> {};
struct ldok_PrecomputedTransactionData : Handle<ldok_PrecomputedTransactionData, PrecomputedTransactionData> {};
struct ldok_BlockHeader: Handle<ldok_BlockHeader, CBlockHeader> {};

ldok_Transaction* ldok_transaction_create(const void* raw_transaction, size_t raw_transaction_len)
{
    if (raw_transaction == nullptr && raw_transaction_len != 0) {
        return nullptr;
    }
    try {
        SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_transaction), raw_transaction_len}};
        return ldok_Transaction::create(std::make_shared<const CTransaction>(deserialize, TX_WITH_WITNESS, stream));
    } catch (...) {
        return nullptr;
    }
}

size_t ldok_transaction_count_outputs(const ldok_Transaction* transaction)
{
    return ldok_Transaction::get(transaction)->vout.size();
}

const ldok_TransactionOutput* ldok_transaction_get_output_at(const ldok_Transaction* transaction, size_t output_index)
{
    const CTransaction& tx = *ldok_Transaction::get(transaction);
    assert(output_index < tx.vout.size());
    return ldok_TransactionOutput::ref(&tx.vout[output_index]);
}

size_t ldok_transaction_count_inputs(const ldok_Transaction* transaction)
{
    return ldok_Transaction::get(transaction)->vin.size();
}

const ldok_TransactionInput* ldok_transaction_get_input_at(const ldok_Transaction* transaction, size_t input_index)
{
    assert(input_index < ldok_Transaction::get(transaction)->vin.size());
    return ldok_TransactionInput::ref(&ldok_Transaction::get(transaction)->vin[input_index]);
}

uint32_t ldok_transaction_get_locktime(const ldok_Transaction* transaction)
{
    return ldok_Transaction::get(transaction)->nLockTime;
}

const ldok_Txid* ldok_transaction_get_txid(const ldok_Transaction* transaction)
{
    return ldok_Txid::ref(&ldok_Transaction::get(transaction)->GetHash());
}

ldok_Transaction* ldok_transaction_copy(const ldok_Transaction* transaction)
{
    return ldok_Transaction::copy(transaction);
}

int ldok_transaction_to_bytes(const ldok_Transaction* transaction, ldok_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(ldok_Transaction::get(transaction));
        return 0;
    } catch (...) {
        return -1;
    }
}

void ldok_transaction_destroy(ldok_Transaction* transaction)
{
    delete transaction;
}

ldok_ScriptPubkey* ldok_script_pubkey_create(const void* script_pubkey, size_t script_pubkey_len)
{
    if (script_pubkey == nullptr && script_pubkey_len != 0) {
        return nullptr;
    }
    auto data = std::span{reinterpret_cast<const uint8_t*>(script_pubkey), script_pubkey_len};
    return ldok_ScriptPubkey::create(data.begin(), data.end());
}

int ldok_script_pubkey_to_bytes(const ldok_ScriptPubkey* script_pubkey_, ldok_WriteBytes writer, void* user_data)
{
    const auto& script_pubkey{ldok_ScriptPubkey::get(script_pubkey_)};
    return writer(script_pubkey.data(), script_pubkey.size(), user_data);
}

ldok_ScriptPubkey* ldok_script_pubkey_copy(const ldok_ScriptPubkey* script_pubkey)
{
    return ldok_ScriptPubkey::copy(script_pubkey);
}

void ldok_script_pubkey_destroy(ldok_ScriptPubkey* script_pubkey)
{
    delete script_pubkey;
}

ldok_TransactionOutput* ldok_transaction_output_create(const ldok_ScriptPubkey* script_pubkey, int64_t amount)
{
    return ldok_TransactionOutput::create(amount, ldok_ScriptPubkey::get(script_pubkey));
}

ldok_TransactionOutput* ldok_transaction_output_copy(const ldok_TransactionOutput* output)
{
    return ldok_TransactionOutput::copy(output);
}

const ldok_ScriptPubkey* ldok_transaction_output_get_script_pubkey(const ldok_TransactionOutput* output)
{
    return ldok_ScriptPubkey::ref(&ldok_TransactionOutput::get(output).scriptPubKey);
}

int64_t ldok_transaction_output_get_amount(const ldok_TransactionOutput* output)
{
    return ldok_TransactionOutput::get(output).nValue;
}

void ldok_transaction_output_destroy(ldok_TransactionOutput* output)
{
    delete output;
}

ldok_PrecomputedTransactionData* ldok_precomputed_transaction_data_create(
    const ldok_Transaction* tx_to,
    const ldok_TransactionOutput** spent_outputs_, size_t spent_outputs_len)
{
    try {
        const CTransaction& tx{*ldok_Transaction::get(tx_to)};
        auto txdata{ldok_PrecomputedTransactionData::create()};
        if (spent_outputs_ != nullptr && spent_outputs_len > 0) {
            assert(spent_outputs_len == tx.vin.size());
            std::vector<CTxOut> spent_outputs;
            spent_outputs.reserve(spent_outputs_len);
            for (size_t i = 0; i < spent_outputs_len; i++) {
                const CTxOut& tx_out{ldok_TransactionOutput::get(spent_outputs_[i])};
                spent_outputs.push_back(tx_out);
            }
            ldok_PrecomputedTransactionData::get(txdata).Init(tx, std::move(spent_outputs));
        } else {
            ldok_PrecomputedTransactionData::get(txdata).Init(tx, {});
        }

        return txdata;
    } catch (...) {
        return nullptr;
    }
}

ldok_PrecomputedTransactionData* ldok_precomputed_transaction_data_copy(const ldok_PrecomputedTransactionData* precomputed_txdata)
{
    return ldok_PrecomputedTransactionData::copy(precomputed_txdata);
}

void ldok_precomputed_transaction_data_destroy(ldok_PrecomputedTransactionData* precomputed_txdata)
{
    delete precomputed_txdata;
}

int ldok_script_pubkey_verify(const ldok_ScriptPubkey* script_pubkey,
                              const int64_t amount,
                              const ldok_Transaction* tx_to,
                              const ldok_PrecomputedTransactionData* precomputed_txdata,
                              const unsigned int input_index,
                              const ldok_ScriptVerificationFlags flags,
                              ldok_ScriptVerifyStatus* status)
{
    // Assert that all specified flags are part of the interface before continuing
    assert((flags & ~ldok_ScriptVerificationFlags_ALL) == 0);

    if (!is_valid_flag_combination(script_verify_flags::from_int(flags))) {
        if (status) *status = ldok_ScriptVerifyStatus_ERROR_INVALID_FLAGS_COMBINATION;
        return 0;
    }

    const CTransaction& tx{*ldok_Transaction::get(tx_to)};
    assert(input_index < tx.vin.size());

    const PrecomputedTransactionData& txdata{precomputed_txdata ? ldok_PrecomputedTransactionData::get(precomputed_txdata) : PrecomputedTransactionData(tx)};

    if (flags & ldok_ScriptVerificationFlags_TAPROOT && txdata.m_spent_outputs.empty()) {
        if (status) *status = ldok_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED;
        return 0;
    }

    if (status) *status = ldok_ScriptVerifyStatus_OK;

    bool result = VerifyScript(tx.vin[input_index].scriptSig,
                               ldok_ScriptPubkey::get(script_pubkey),
                               &tx.vin[input_index].scriptWitness,
                               script_verify_flags::from_int(flags),
                               TransactionSignatureChecker(&tx, input_index, amount, txdata, MissingDataBehavior::FAIL),
                               nullptr);
    return result ? 1 : 0;
}

ldok_TransactionInput* ldok_transaction_input_copy(const ldok_TransactionInput* input)
{
    return ldok_TransactionInput::copy(input);
}

const ldok_TransactionOutPoint* ldok_transaction_input_get_out_point(const ldok_TransactionInput* input)
{
    return ldok_TransactionOutPoint::ref(&ldok_TransactionInput::get(input).prevout);
}

uint32_t ldok_transaction_input_get_sequence(const ldok_TransactionInput* input)
{
    return ldok_TransactionInput::get(input).nSequence;
}

void ldok_transaction_input_destroy(ldok_TransactionInput* input)
{
    delete input;
}

ldok_TransactionOutPoint* ldok_transaction_out_point_copy(const ldok_TransactionOutPoint* out_point)
{
    return ldok_TransactionOutPoint::copy(out_point);
}

uint32_t ldok_transaction_out_point_get_index(const ldok_TransactionOutPoint* out_point)
{
    return ldok_TransactionOutPoint::get(out_point).n;
}

const ldok_Txid* ldok_transaction_out_point_get_txid(const ldok_TransactionOutPoint* out_point)
{
    return ldok_Txid::ref(&ldok_TransactionOutPoint::get(out_point).hash);
}

void ldok_transaction_out_point_destroy(ldok_TransactionOutPoint* out_point)
{
    delete out_point;
}

ldok_Txid* ldok_txid_copy(const ldok_Txid* txid)
{
    return ldok_Txid::copy(txid);
}

void ldok_txid_to_bytes(const ldok_Txid* txid, unsigned char output[32])
{
    std::memcpy(output, ldok_Txid::get(txid).begin(), 32);
}

int ldok_txid_equals(const ldok_Txid* txid1, const ldok_Txid* txid2)
{
    return ldok_Txid::get(txid1) == ldok_Txid::get(txid2);
}

void ldok_txid_destroy(ldok_Txid* txid)
{
    delete txid;
}

void ldok_logging_set_options(const ldok_LoggingOptions options)
{
    LOCK(cs_main);
    LogInstance().m_log_timestamps = options.log_timestamps;
    LogInstance().m_log_time_micros = options.log_time_micros;
    LogInstance().m_log_threadnames = options.log_threadnames;
    LogInstance().m_log_sourcelocations = options.log_sourcelocations;
    LogInstance().m_always_print_category_level = options.always_print_category_levels;
}

void ldok_logging_set_level_category(ldok_LogCategory category, ldok_LogLevel level)
{
    LOCK(cs_main);
    if (category == ldok_LogCategory_ALL) {
        LogInstance().SetLogLevel(get_bclog_level(level));
    }

    LogInstance().AddCategoryLogLevel(get_bclog_flag(category), get_bclog_level(level));
}

void ldok_logging_enable_category(ldok_LogCategory category)
{
    LogInstance().EnableCategory(get_bclog_flag(category));
}

void ldok_logging_disable_category(ldok_LogCategory category)
{
    LogInstance().DisableCategory(get_bclog_flag(category));
}

void ldok_logging_disable()
{
    LogInstance().DisableLogging();
}

ldok_LoggingConnection* ldok_logging_connection_create(ldok_LogCallback callback, void* user_data, ldok_DestroyCallback user_data_destroy_callback)
{
    try {
        return ldok_LoggingConnection::create(callback, user_data, user_data_destroy_callback);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void ldok_logging_connection_destroy(ldok_LoggingConnection* connection)
{
    delete connection;
}

ldok_ChainParameters* ldok_chain_parameters_create(const ldok_ChainType chain_type)
{
    switch (chain_type) {
    case ldok_ChainType_MAINNET: {
        return ldok_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::Main().release()));
    }
    case ldok_ChainType_TESTNET: {
        return ldok_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet().release()));
    }
    case ldok_ChainType_TESTNET_4: {
        return ldok_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet4().release()));
    }
    case ldok_ChainType_SIGNET: {
        return ldok_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::SigNet({}).release()));
    }
    case ldok_ChainType_REGTEST: {
        return ldok_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::RegTest({}).release()));
    }
    }
    assert(false);
}

ldok_ChainParameters* ldok_chain_parameters_copy(const ldok_ChainParameters* chain_parameters)
{
    return ldok_ChainParameters::copy(chain_parameters);
}

void ldok_chain_parameters_destroy(ldok_ChainParameters* chain_parameters)
{
    delete chain_parameters;
}

ldok_ContextOptions* ldok_context_options_create()
{
    return ldok_ContextOptions::create();
}

void ldok_context_options_set_chainparams(ldok_ContextOptions* options, const ldok_ChainParameters* chain_parameters)
{
    // Copy the chainparams, so the caller can free it again
    LOCK(ldok_ContextOptions::get(options).m_mutex);
    ldok_ContextOptions::get(options).m_chainparams = std::make_unique<const CChainParams>(ldok_ChainParameters::get(chain_parameters));
}

void ldok_context_options_set_notifications(ldok_ContextOptions* options, ldok_NotificationInterfaceCallbacks notifications)
{
    // The KernelNotifications are copy-initialized, so the caller can free them again.
    LOCK(ldok_ContextOptions::get(options).m_mutex);
    ldok_ContextOptions::get(options).m_notifications = std::make_shared<KernelNotifications>(notifications);
}

void ldok_context_options_set_validation_interface(ldok_ContextOptions* options, ldok_ValidationInterfaceCallbacks vi_cbs)
{
    LOCK(ldok_ContextOptions::get(options).m_mutex);
    ldok_ContextOptions::get(options).m_validation_interface = std::make_shared<KernelValidationInterface>(vi_cbs);
}

void ldok_context_options_destroy(ldok_ContextOptions* options)
{
    delete options;
}

ldok_Context* ldok_context_create(const ldok_ContextOptions* options)
{
    bool sane{true};
    const ContextOptions* opts = options ? &ldok_ContextOptions::get(options) : nullptr;
    auto context{std::make_shared<const Context>(opts, sane)};
    if (!sane) {
        LogError("Kernel context sanity check failed.");
        return nullptr;
    }
    return ldok_Context::create(context);
}

ldok_Context* ldok_context_copy(const ldok_Context* context)
{
    return ldok_Context::copy(context);
}

int ldok_context_interrupt(ldok_Context* context)
{
    return (*ldok_Context::get(context)->m_interrupt)() ? 0 : -1;
}

void ldok_context_destroy(ldok_Context* context)
{
    delete context;
}

const ldok_BlockTreeEntry* ldok_block_tree_entry_get_previous(const ldok_BlockTreeEntry* entry)
{
    if (!ldok_BlockTreeEntry::get(entry).pprev) {
        LogInfo("Genesis block has no previous.");
        return nullptr;
    }

    return ldok_BlockTreeEntry::ref(ldok_BlockTreeEntry::get(entry).pprev);
}

ldok_BlockValidationState* ldok_block_validation_state_create()
{
    return ldok_BlockValidationState::create();
}

ldok_BlockValidationState* ldok_block_validation_state_copy(const ldok_BlockValidationState* state)
{
    return ldok_BlockValidationState::copy(state);
}

void ldok_block_validation_state_destroy(ldok_BlockValidationState* state)
{
    delete state;
}

ldok_ValidationMode ldok_block_validation_state_get_validation_mode(const ldok_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = ldok_BlockValidationState::get(block_validation_state_);
    if (block_validation_state.IsValid()) return ldok_ValidationMode_VALID;
    if (block_validation_state.IsInvalid()) return ldok_ValidationMode_INVALID;
    return ldok_ValidationMode_INTERNAL_ERROR;
}

ldok_BlockValidationResult ldok_block_validation_state_get_block_validation_result(const ldok_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = ldok_BlockValidationState::get(block_validation_state_);
    switch (block_validation_state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        return ldok_BlockValidationResult_UNSET;
    case BlockValidationResult::BLOCK_CONSENSUS:
        return ldok_BlockValidationResult_CONSENSUS;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        return ldok_BlockValidationResult_CACHED_INVALID;
    case BlockValidationResult::BLOCK_INVALID_HEADER:
        return ldok_BlockValidationResult_INVALID_HEADER;
    case BlockValidationResult::BLOCK_MUTATED:
        return ldok_BlockValidationResult_MUTATED;
    case BlockValidationResult::BLOCK_MISSING_PREV:
        return ldok_BlockValidationResult_MISSING_PREV;
    case BlockValidationResult::BLOCK_INVALID_PREV:
        return ldok_BlockValidationResult_INVALID_PREV;
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        return ldok_BlockValidationResult_TIME_FUTURE;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        return ldok_BlockValidationResult_HEADER_LOW_WORK;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

ldok_ChainstateManagerOptions* ldok_chainstate_manager_options_create(const ldok_Context* context, const char* data_dir, size_t data_dir_len, const char* blocks_dir, size_t blocks_dir_len)
{
    if (data_dir == nullptr || data_dir_len == 0 || blocks_dir == nullptr || blocks_dir_len == 0) {
        LogError("Failed to create chainstate manager options: dir must be non-null and non-empty");
        return nullptr;
    }
    try {
        fs::path abs_data_dir{fs::absolute(fs::PathFromString({data_dir, data_dir_len}))};
        fs::create_directories(abs_data_dir);
        fs::path abs_blocks_dir{fs::absolute(fs::PathFromString({blocks_dir, blocks_dir_len}))};
        fs::create_directories(abs_blocks_dir);
        return ldok_ChainstateManagerOptions::create(ldok_Context::get(context), abs_data_dir, abs_blocks_dir);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager options: %s", e.what());
        return nullptr;
    }
}

void ldok_chainstate_manager_options_set_worker_threads_num(ldok_ChainstateManagerOptions* opts, int worker_threads)
{
    LOCK(ldok_ChainstateManagerOptions::get(opts).m_mutex);
    ldok_ChainstateManagerOptions::get(opts).m_chainman_options.worker_threads_num = worker_threads;
}

void ldok_chainstate_manager_options_destroy(ldok_ChainstateManagerOptions* options)
{
    delete options;
}

int ldok_chainstate_manager_options_set_wipe_dbs(ldok_ChainstateManagerOptions* chainman_opts, int wipe_block_tree_db, int wipe_chainstate_db)
{
    if (wipe_block_tree_db == 1 && wipe_chainstate_db != 1) {
        LogError("Wiping the block tree db without also wiping the chainstate db is currently unsupported.");
        return -1;
    }
    auto& opts{ldok_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.wipe_data = wipe_block_tree_db == 1;
    opts.m_chainstate_load_options.wipe_chainstate_db = wipe_chainstate_db == 1;
    return 0;
}

void ldok_chainstate_manager_options_update_block_tree_db_in_memory(
    ldok_ChainstateManagerOptions* chainman_opts,
    int block_tree_db_in_memory)
{
    auto& opts{ldok_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.memory_only = block_tree_db_in_memory == 1;
}

void ldok_chainstate_manager_options_update_chainstate_db_in_memory(
    ldok_ChainstateManagerOptions* chainman_opts,
    int chainstate_db_in_memory)
{
    auto& opts{ldok_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_chainstate_load_options.coins_db_in_memory = chainstate_db_in_memory == 1;
}

ldok_ChainstateManager* ldok_chainstate_manager_create(
    const ldok_ChainstateManagerOptions* chainman_opts)
{
    auto& opts{ldok_ChainstateManagerOptions::get(chainman_opts)};
    std::unique_ptr<ChainstateManager> chainman;
    try {
        LOCK(opts.m_mutex);
        chainman = std::make_unique<ChainstateManager>(*opts.m_context->m_interrupt, opts.m_chainman_options, opts.m_blockman_options);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager: %s", e.what());
        return nullptr;
    }

    try {
        const auto chainstate_load_opts{WITH_LOCK(opts.m_mutex, return opts.m_chainstate_load_options)};

        kernel::CacheSizes cache_sizes{DEFAULT_KERNEL_CACHE};
        auto [status, chainstate_err]{node::LoadChainstate(*chainman, cache_sizes, chainstate_load_opts)};
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to load chain state from your data directory: %s", chainstate_err.original);
            return nullptr;
        }
        std::tie(status, chainstate_err) = node::VerifyLoadedChainstate(*chainman, chainstate_load_opts);
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to verify loaded chain state from your datadir: %s", chainstate_err.original);
            return nullptr;
        }
        if (auto result = chainman->ActivateBestChains(); !result) {
            LogError("%s", util::ErrorString(result).original);
            return nullptr;
        }
    } catch (const std::exception& e) {
        LogError("Failed to load chainstate: %s", e.what());
        return nullptr;
    }

    return ldok_ChainstateManager::create(std::move(chainman), opts.m_context);
}

const ldok_BlockTreeEntry* ldok_chainstate_manager_get_block_tree_entry_by_hash(const ldok_ChainstateManager* chainman, const ldok_BlockHash* block_hash)
{
    auto block_index = WITH_LOCK(ldok_ChainstateManager::get(chainman).m_chainman->GetMutex(),
                                 return ldok_ChainstateManager::get(chainman).m_chainman->m_blockman.LookupBlockIndex(ldok_BlockHash::get(block_hash)));
    if (!block_index) {
        LogDebug(BCLog::KERNEL, "A block with the given hash is not indexed.");
        return nullptr;
    }
    return ldok_BlockTreeEntry::ref(block_index);
}

const ldok_BlockTreeEntry* ldok_chainstate_manager_get_best_entry(const ldok_ChainstateManager* chainstate_manager)
{
    auto& chainman = *ldok_ChainstateManager::get(chainstate_manager).m_chainman;
    return ldok_BlockTreeEntry::ref(WITH_LOCK(chainman.GetMutex(), return chainman.m_best_header));
}

void ldok_chainstate_manager_destroy(ldok_ChainstateManager* chainman)
{
    {
        LOCK(ldok_ChainstateManager::get(chainman).m_chainman->GetMutex());
        for (const auto& chainstate : ldok_ChainstateManager::get(chainman).m_chainman->m_chainstates) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }

    delete chainman;
}

int ldok_chainstate_manager_import_blocks(ldok_ChainstateManager* chainman, const char** block_file_paths_data, size_t* block_file_paths_lens, size_t block_file_paths_data_len)
{
    try {
        std::vector<fs::path> import_files;
        import_files.reserve(block_file_paths_data_len);
        for (uint32_t i = 0; i < block_file_paths_data_len; i++) {
            if (block_file_paths_data[i] != nullptr) {
                import_files.emplace_back(std::string{block_file_paths_data[i], block_file_paths_lens[i]}.c_str());
            }
        }
        auto& chainman_ref{*ldok_ChainstateManager::get(chainman).m_chainman};
        node::ImportBlocks(chainman_ref, import_files);
        WITH_LOCK(::cs_main, chainman_ref.UpdateIBDStatus());
    } catch (const std::exception& e) {
        LogError("Failed to import blocks: %s", e.what());
        return -1;
    }
    return 0;
}

ldok_Block* ldok_block_create(const void* raw_block, size_t raw_block_length)
{
    if (raw_block == nullptr && raw_block_length != 0) {
        return nullptr;
    }
    auto block{std::make_shared<CBlock>()};

    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block), raw_block_length}};

    try {
        stream >> TX_WITH_WITNESS(*block);
    } catch (...) {
        LogDebug(BCLog::KERNEL, "Block decode failed.");
        return nullptr;
    }

    return ldok_Block::create(block);
}

ldok_Block* ldok_block_copy(const ldok_Block* block)
{
    return ldok_Block::copy(block);
}

size_t ldok_block_count_transactions(const ldok_Block* block)
{
    return ldok_Block::get(block)->vtx.size();
}

const ldok_Transaction* ldok_block_get_transaction_at(const ldok_Block* block, size_t index)
{
    assert(index < ldok_Block::get(block)->vtx.size());
    return ldok_Transaction::ref(&ldok_Block::get(block)->vtx[index]);
}

ldok_BlockHeader* ldok_block_get_header(const ldok_Block* block)
{
    const auto& block_ptr = ldok_Block::get(block);
    return ldok_BlockHeader::create(static_cast<const CBlockHeader&>(*block_ptr));
}

int ldok_block_to_bytes(const ldok_Block* block, ldok_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(*ldok_Block::get(block));
        return 0;
    } catch (...) {
        return -1;
    }
}

ldok_BlockHash* ldok_block_get_hash(const ldok_Block* block)
{
    return ldok_BlockHash::create(ldok_Block::get(block)->GetHash());
}

void ldok_block_destroy(ldok_Block* block)
{
    delete block;
}

ldok_Block* ldok_block_read(const ldok_ChainstateManager* chainman, const ldok_BlockTreeEntry* entry)
{
    auto block{std::make_shared<CBlock>()};
    if (!ldok_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlock(*block, ldok_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block.");
        return nullptr;
    }
    return ldok_Block::create(block);
}

ldok_BlockHeader* ldok_block_tree_entry_get_block_header(const ldok_BlockTreeEntry* entry)
{
    return ldok_BlockHeader::create(ldok_BlockTreeEntry::get(entry).GetBlockHeader());
}

int32_t ldok_block_tree_entry_get_height(const ldok_BlockTreeEntry* entry)
{
    return ldok_BlockTreeEntry::get(entry).nHeight;
}

const ldok_BlockHash* ldok_block_tree_entry_get_block_hash(const ldok_BlockTreeEntry* entry)
{
    return ldok_BlockHash::ref(ldok_BlockTreeEntry::get(entry).phashBlock);
}

int ldok_block_tree_entry_equals(const ldok_BlockTreeEntry* entry1, const ldok_BlockTreeEntry* entry2)
{
    return &ldok_BlockTreeEntry::get(entry1) == &ldok_BlockTreeEntry::get(entry2);
}

ldok_BlockHash* ldok_block_hash_create(const unsigned char block_hash[32])
{
    return ldok_BlockHash::create(std::span<const unsigned char>{block_hash, 32});
}

ldok_BlockHash* ldok_block_hash_copy(const ldok_BlockHash* block_hash)
{
    return ldok_BlockHash::copy(block_hash);
}

void ldok_block_hash_to_bytes(const ldok_BlockHash* block_hash, unsigned char output[32])
{
    std::memcpy(output, ldok_BlockHash::get(block_hash).begin(), 32);
}

int ldok_block_hash_equals(const ldok_BlockHash* hash1, const ldok_BlockHash* hash2)
{
    return ldok_BlockHash::get(hash1) == ldok_BlockHash::get(hash2);
}

void ldok_block_hash_destroy(ldok_BlockHash* hash)
{
    delete hash;
}

ldok_BlockSpentOutputs* ldok_block_spent_outputs_read(const ldok_ChainstateManager* chainman, const ldok_BlockTreeEntry* entry)
{
    auto block_undo{std::make_shared<CBlockUndo>()};
    if (ldok_BlockTreeEntry::get(entry).nHeight < 1) {
        LogDebug(BCLog::KERNEL, "The genesis block does not have any spent outputs.");
        return ldok_BlockSpentOutputs::create(block_undo);
    }
    if (!ldok_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlockUndo(*block_undo, ldok_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block spent outputs data.");
        return nullptr;
    }
    return ldok_BlockSpentOutputs::create(block_undo);
}

ldok_BlockSpentOutputs* ldok_block_spent_outputs_copy(const ldok_BlockSpentOutputs* block_spent_outputs)
{
    return ldok_BlockSpentOutputs::copy(block_spent_outputs);
}

size_t ldok_block_spent_outputs_count(const ldok_BlockSpentOutputs* block_spent_outputs)
{
    return ldok_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size();
}

const ldok_TransactionSpentOutputs* ldok_block_spent_outputs_get_transaction_spent_outputs_at(const ldok_BlockSpentOutputs* block_spent_outputs, size_t transaction_index)
{
    assert(transaction_index < ldok_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size());
    const auto* tx_undo{&ldok_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.at(transaction_index)};
    return ldok_TransactionSpentOutputs::ref(tx_undo);
}

void ldok_block_spent_outputs_destroy(ldok_BlockSpentOutputs* block_spent_outputs)
{
    delete block_spent_outputs;
}

ldok_TransactionSpentOutputs* ldok_transaction_spent_outputs_copy(const ldok_TransactionSpentOutputs* transaction_spent_outputs)
{
    return ldok_TransactionSpentOutputs::copy(transaction_spent_outputs);
}

size_t ldok_transaction_spent_outputs_count(const ldok_TransactionSpentOutputs* transaction_spent_outputs)
{
    return ldok_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size();
}

void ldok_transaction_spent_outputs_destroy(ldok_TransactionSpentOutputs* transaction_spent_outputs)
{
    delete transaction_spent_outputs;
}

const ldok_Coin* ldok_transaction_spent_outputs_get_coin_at(const ldok_TransactionSpentOutputs* transaction_spent_outputs, size_t coin_index)
{
    assert(coin_index < ldok_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size());
    const Coin* coin{&ldok_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.at(coin_index)};
    return ldok_Coin::ref(coin);
}

ldok_Coin* ldok_coin_copy(const ldok_Coin* coin)
{
    return ldok_Coin::copy(coin);
}

uint32_t ldok_coin_confirmation_height(const ldok_Coin* coin)
{
    return ldok_Coin::get(coin).nHeight;
}

int ldok_coin_is_coinbase(const ldok_Coin* coin)
{
    return ldok_Coin::get(coin).IsCoinBase() ? 1 : 0;
}

const ldok_TransactionOutput* ldok_coin_get_output(const ldok_Coin* coin)
{
    return ldok_TransactionOutput::ref(&ldok_Coin::get(coin).out);
}

void ldok_coin_destroy(ldok_Coin* coin)
{
    delete coin;
}

int ldok_chainstate_manager_process_block(
    ldok_ChainstateManager* chainman,
    const ldok_Block* block,
    int* _new_block)
{
    bool new_block;
    auto result = ldok_ChainstateManager::get(chainman).m_chainman->ProcessNewBlock(ldok_Block::get(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    if (_new_block) {
        *_new_block = new_block ? 1 : 0;
    }
    return result ? 0 : -1;
}

int ldok_chainstate_manager_process_block_header(
    ldok_ChainstateManager* chainstate_manager,
    const ldok_BlockHeader* header,
    ldok_BlockValidationState* state)
{
    try {
        auto& chainman = ldok_ChainstateManager::get(chainstate_manager).m_chainman;
        auto result = chainman->ProcessNewBlockHeaders({&ldok_BlockHeader::get(header), 1}, /*min_pow_checked=*/true, ldok_BlockValidationState::get(state), /*ppindex=*/nullptr);

        return result ? 0 : -1;
    } catch (const std::exception& e) {
        LogError("Failed to process block header: %s", e.what());
        return -1;
    }
}

const ldok_Chain* ldok_chainstate_manager_get_active_chain(const ldok_ChainstateManager* chainman)
{
    return ldok_Chain::ref(&WITH_LOCK(ldok_ChainstateManager::get(chainman).m_chainman->GetMutex(), return ldok_ChainstateManager::get(chainman).m_chainman->ActiveChain()));
}

int ldok_chain_get_height(const ldok_Chain* chain)
{
    LOCK(::cs_main);
    return ldok_Chain::get(chain).Height();
}

const ldok_BlockTreeEntry* ldok_chain_get_by_height(const ldok_Chain* chain, int height)
{
    LOCK(::cs_main);
    return ldok_BlockTreeEntry::ref(ldok_Chain::get(chain)[height]);
}

int ldok_chain_contains(const ldok_Chain* chain, const ldok_BlockTreeEntry* entry)
{
    LOCK(::cs_main);
    return ldok_Chain::get(chain).Contains(&ldok_BlockTreeEntry::get(entry)) ? 1 : 0;
}

ldok_BlockHeader* ldok_block_header_create(const void* raw_block_header, size_t raw_block_header_len)
{
    if (raw_block_header == nullptr && raw_block_header_len != 0) {
        return nullptr;
    }
    auto header{std::make_unique<CBlockHeader>()};
    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block_header), raw_block_header_len}};

    try {
        stream >> *header;
    } catch (...) {
        LogError("Block header decode failed.");
        return nullptr;
    }

    return ldok_BlockHeader::ref(header.release());
}

ldok_BlockHeader* ldok_block_header_copy(const ldok_BlockHeader* header)
{
    return ldok_BlockHeader::copy(header);
}

ldok_BlockHash* ldok_block_header_get_hash(const ldok_BlockHeader* header)
{
    return ldok_BlockHash::create(ldok_BlockHeader::get(header).GetHash());
}

const ldok_BlockHash* ldok_block_header_get_prev_hash(const ldok_BlockHeader* header)
{
    return ldok_BlockHash::ref(&ldok_BlockHeader::get(header).hashPrevBlock);
}

uint32_t ldok_block_header_get_timestamp(const ldok_BlockHeader* header)
{
    return ldok_BlockHeader::get(header).nTime;
}

uint32_t ldok_block_header_get_bits(const ldok_BlockHeader* header)
{
    return ldok_BlockHeader::get(header).nBits;
}

int32_t ldok_block_header_get_version(const ldok_BlockHeader* header)
{
    return ldok_BlockHeader::get(header).nVersion;
}

uint32_t ldok_block_header_get_nonce(const ldok_BlockHeader* header)
{
    return ldok_BlockHeader::get(header).nNonce;
}

void ldok_block_header_destroy(ldok_BlockHeader* header)
{
    delete header;
}
