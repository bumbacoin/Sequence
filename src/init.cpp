// Copyright (c) 2009-2019 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/sequence-config.h"
#endif

#include "init.h"

#include "addrman.h"
#include "amount.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "consensus/consensus.h"
#include "hooks.h"
#include "key.h"
#include "main.h"
#include "miner.h"
#include "net.h"
#include "ntp.h"       
#include "rpc/rpcregister.h"
#include "rpc/rpcserver.h"
#include "compat/sanity.h"
#include "scheduler.h"
#include "dns/seqdns.h"
#include "script/standard.h"
#include "torcontrol.h"
#include "txdb.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "consensus/validation.h"
#include "validationinterface.h"

#ifdef ENABLE_WALLET
#include "wallet/db.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <stdint.h>
#include <stdio.h>

#ifndef WIN32
#include <signal.h>
#endif

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>

using namespace boost;
using namespace std;

#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
#endif
bool fFeeEstimatesInitialized = false;
bool fRestartRequested = false; // true: restart, false: shutdown
SeqDns* seqdns = NULL;

static const bool DEFAULT_PROXYRANDOMIZE = true;

#ifdef WIN32
// Win32 LevelDB doesn't use filedescriptors, and the ones used for
// accessing block files, don't count towards to fd_set size limit
// anyway.
#define MIN_CORE_FILEDESCRIPTORS 0
#else
#define MIN_CORE_FILEDESCRIPTORS 150
#endif

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE         = 0,
    BF_EXPLICIT     = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    BF_WHITELIST    = (1U << 2),
};

static const char* FEE_ESTIMATES_FILENAME="fee_estimates.dat";
CClientUIInterface uiInterface;

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//
//
//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

volatile bool fRequestShutdown = false;

void StartShutdown()
{
    fRequestShutdown = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown || fRestartRequested;
}

class CCoinsViewErrorCatcher : public CCoinsViewBacked
{
public:
    CCoinsViewErrorCatcher(CCoinsView* view) : CCoinsViewBacked(view) {}
    bool GetCoins(const uint256 &txid, CCoins &coins) const {
        try {
            return CCoinsViewBacked::GetCoins(txid, coins);
        } catch(const std::runtime_error& e) {
            uiInterface.ThreadSafeMessageBox(_("Error reading from database, shutting down."), "", CClientUIInterface::MSG_ERROR);
            LogPrintf("Error reading from database: %s\n", e.what());
            // Starting the shutdown sequence and returning false to the caller would be
            // interpreted as 'entry not found' (as opposed to unable to read data), and
            // could lead to invalid interpration. Just exit immediately, as we can't
            // continue anyway, and all writes should be atomic.
            abort();
        }
    }
    // Writes do not need similar protection, as failure to write is handled by the caller.
};

static CCoinsViewDB *pcoinsdbview = NULL;
static CCoinsViewErrorCatcher *pcoinscatcher = NULL;

/** Preparing steps before shutting down or restarting the wallet */
void PrepareShutdown()
{
    fRequestShutdown = true; // Needed when we shutdown the wallet
    fRestartRequested = true; // Needed when we restart the wallet
    LogPrintf("%s: In progress...\n", __func__);
    if (seqdns)
        delete seqdns;
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown)
        return;

    /// Note: Shutdown() must be able to handle cases in which AppInit2() failed part of the way,
    /// for example if the data directory was found to be locked.
    /// Be sure that anything that writes files or flushes caches only does this if the respective
    /// module was initialized.
    RenameThread("sequence-shutoff");
    mempool.AddTransactionsUpdated(1);
    StopRPCThreads();
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(false);
    ShutdownRPCMining();
    GenerateSequences(false, NULL, 0, Params());
#endif
    StopNode();
	StopTorControl();

    UnregisterNodeSignals(GetNodeSignals());

    if (fFeeEstimatesInitialized)
    {
        boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
        CAutoFile est_fileout(fopen(est_path.string().c_str(), "wb"), SER_DISK, CLIENT_VERSION);
        if (!est_fileout.IsNull())
            mempool.WriteFeeEstimates(est_fileout);
        else
            LogPrintf("%s: Failed to write fee estimates to %s\n", __func__, est_path.string());
        fFeeEstimatesInitialized = false;
    }

    {
        LOCK(cs_main);
        if (pcoinsTip != NULL) {
            FlushStateToDisk();
        }
        delete pcoinsTip;
        pcoinsTip = NULL;
        delete pcoinscatcher;
        pcoinscatcher = NULL;
        delete pcoinsdbview;
        pcoinsdbview = NULL;
        delete pblocktree;
        pblocktree = NULL;
    }
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(true);
#endif
#ifndef WIN32
    boost::filesystem::remove(GetPidFile());
#endif
    UnregisterAllValidationInterfaces();
}

/**
* Shutdown is split into 2 parts:
* Part 1: shut down everything but the main wallet instance (done in PrepareShutdown() )
* Part 2: delete wallet instance
*
* In case of a restart PrepareShutdown() was already called before, but this method here gets
* called implicitly when the parent object is deleted. In this case we have to skip the
* PrepareShutdown() part because it was already executed and just delete the wallet instance.
*/
void Shutdown()
{
    // Shutdown part 1: prepare shutdown
    if(!fRestartRequested){
        PrepareShutdown();
    }

   // Shutdown part 2: delete wallet instance
#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif
    LogPrintf("%s: done\n", __func__);
}

/**
 * Signal handlers are very limited in what they are allowed to do, so:
 */
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

bool static InitError(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_ERROR);
    return false;
}

bool static InitWarning(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_WARNING);
    return true;
}

bool static Bind(const CService &addr, unsigned int flags) {
    if (!(flags & BF_EXPLICIT) && IsLimited(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError, (flags & BF_WHITELIST) != 0)) {
        if (flags & BF_REPORT_ERROR)
            return InitError(strError);
        return false;
    }
    return true;
}

std::string HelpMessage(HelpMessageMode mode)
{
    // When adding new options to the categories, please keep and ensure alphabetical ordering.
    string strUsage = _("Options:") + "\n";
    strUsage += "  -?                     " + _("This help message") + "\n";
    strUsage += "  -alertnotify=<cmd>     " + _("Execute command when a relevant alert is received or we see a really long fork (%s in cmd is replaced by message)") + "\n";
    strUsage += "  -alerts                " + strprintf(_("Receive and display P2P network alerts (default: %u)"), DEFAULT_ALERTS);
    strUsage += "  -blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n";
    strUsage += "  -checkblocks=<n>       " + strprintf(_("How many blocks to check at startup (default: %u, 0 = all)"), 288) + "\n";
    strUsage += "  -checklevel=<n>        " + strprintf(_("How thorough the block verification of -checkblocks is (0-4, default: %u)"), 3) + "\n";
    strUsage += "  -conf=<file>           " + strprintf(_("Specify configuration file (default: %s)"), "sequence.conf") + "\n";
    if (mode == HMM_SEQUENCED)
    {
#if !defined(WIN32)
        strUsage += "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
#endif
    }
    strUsage += "  -datadir=<dir>         " + _("Specify data directory") + "\n";
    strUsage += "  -dbcache=<n>           " + strprintf(_("Set database cache size in megabytes (%d to %d, default: %d)"), nMinDbCache, nMaxDbCache, nDefaultDbCache) + "\n";
    strUsage += "  -loadblock=<file>      " + _("Imports blocks from external blk000??.dat file") + " " + _("on startup") + "\n";
    strUsage += "  -maxorphantx=<n>       " + strprintf(_("Keep at most <n> unconnectable transactions in memory (default: %u)"), DEFAULT_MAX_ORPHAN_TRANSACTIONS) + "\n";
    strUsage += "  -par=<n>               " + strprintf(_("Set the number of script verification threads (%u to %d, 0 = auto, <0 = leave that many cores free, default: %d)"), -GetNumCores(), MAX_SCRIPTCHECK_THREADS, DEFAULT_SCRIPTCHECK_THREADS) + "\n";
#ifndef WIN32
    strUsage += "  -pid=<file>            " + strprintf(_("Specify pid file (default: %s)"), "sequenced.pid") + "\n";
#endif
    strUsage += "  -reindex               " + _("Rebuild block chain index from current blk000??.dat files") + " " + _("on startup") + "\n";
#if !defined(WIN32)
    strUsage += "  -sysperms              " + _("Create new files with system default permissions, instead of umask 077 (only effective with disabled wallet functionality)") + "\n";
#endif
    strUsage += "  -txindex               " + strprintf(_("Maintain a full transaction index, used by the getrawtransaction rpc call (default: %u)"), 0) + "\n";

    strUsage += "\n" + _("Connection options:") + "\n";
    strUsage += "  -addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n";
    strUsage += "  -banscore=<n>          " + strprintf(_("Threshold for disconnecting misbehaving peers (default: %u)"), 100) + "\n";
    strUsage += "  -bantime=<n>           " + strprintf(_("Number of seconds to keep misbehaving peers from reconnecting (default: %u)"), 86400) + "\n";
    strUsage += "  -bind=<addr>           " + _("Bind to given address and always listen on it. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -connect=<ip>          " + _("Connect only to the specified node(s)") + "\n";
    strUsage += "  -discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n";
    strUsage += "  -dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + " " + _("(default: 1)") + "\n";
    strUsage += "  -dnsseed               " + _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect)") + "\n";
    strUsage += "  -externalip=<ip>       " + _("Specify your own public address") + "\n";
    strUsage += "  -forcednsseed          " + strprintf(_("Always query for peer addresses via DNS lookup (default: %u)"), 0) + "\n";
    strUsage += "  -listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n";
    strUsage += "  -listenonion			  " + strprintf(_("Automatically create Tor hidden service (default: %d)"), DEFAULT_LISTEN_ONION);
    strUsage += "  -maxconnections=<n>    " + strprintf(_("Maintain at most <n> connections to peers (default: %u)"), 125) + "\n";
    strUsage += "  -maxreceivebuffer=<n>  " + strprintf(_("Maximum per-connection receive buffer, <n>*1000 bytes (default: %u)"), 5000) + "\n";
    strUsage += "  -maxsendbuffer=<n>     " + strprintf(_("Maximum per-connection send buffer, <n>*1000 bytes (default: %u)"), 1000) + "\n";
    strUsage += "  -onion=<ip:port>       " + strprintf(_("Use separate SOCKS5 proxy to reach peers via Tor hidden services (default: %s)"), "-proxy") + "\n";
    strUsage += "  -onlynet=<net>         " + _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)") + "\n";
    strUsage += "  -permitbaremultisig    " + strprintf(_("Relay non-P2SH multisig (default: %u)"), 1) + "\n";
    strUsage += "  -port=<port>           " + strprintf(_("Listen for connections on <port> (default: %u or testnet: %u)"), Params(CBaseChainParams::MAIN).GetDefaultPort(), Params(CBaseChainParams::TESTNET).GetDefaultPort());
    strUsage += "  -proxy=<ip:port>       " + _("Connect through SOCKS5 proxy") + "\n";
    strUsage += "  -proxyrandomize        " + strprintf(_("Randomize credentials for every proxy connection. This enables Tor stream isolation (default: %u)"), DEFAULT_PROXYRANDOMIZE) + "\n";
    strUsage += "  -seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n";
    strUsage += "  -timeout=<n>           " + strprintf(_("Specify connection timeout in milliseconds (minimum: 1, default: %d)"), DEFAULT_CONNECT_TIMEOUT) + "\n";
    strUsage += "  -torcontrol=<ip>:<port>" + strprintf(_("Tor control port to use if onion listening enabled (default: %s)"), DEFAULT_TOR_CONTROL) + "\n";
    strUsage += "  -torpassword=<pass>	  " + _("Tor control port password (default: empty)") + "\n";

#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n";
#else
    strUsage += "  -upnp                  " + strprintf(_("Use UPnP to map the listening port (default: %u)"), 0) + "\n";
#endif
#endif
    strUsage += "  -whitebind=<addr>      " + _("Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -whitelist=<netmask>   " + _("Whitelist peers connecting from the given netmask or IP address. Can be specified multiple times.") + "\n";
    strUsage += "                         " + _("Whitelisted peers cannot be DoS banned and their transactions are always relayed, even if they are already in the mempool, useful e.g. for a gateway") + "\n";
    strUsage += "  -whitelistrelay        " + strprintf(_("Accept relayed transactions received from whitelisted peers even when not relaying transactions (default: %d)"), DEFAULT_WHITELISTRELAY);
    strUsage += "  -whitelistforcerelay   " + strprintf(_("Force relay of transactions from whitelisted peers even they violate local relay policy (default: %d)"), DEFAULT_WHITELISTFORCERELAY);
    strUsage += "  -maxuploadtarget=<n>   " + strprintf(_("Tries to keep outbound traffic under the given target (in MiB per 24h), 0 = no limit (default: %d)"), DEFAULT_MAX_UPLOAD_TARGET);

#ifdef ENABLE_WALLET
    strUsage += "\n" + _("Wallet options:") + "\n";
    strUsage += "  -disablewallet         " + _("Do not load the wallet and disable wallet RPC calls") + "\n";
    strUsage += "  -keypool=<n>           " + strprintf(_("Set key pool size to <n> (default: %u)"), 100) + "\n";
    if (GetBoolArg("-help-debug", false))
        strUsage += "  -mintxfee=<amt>        " + strprintf(_("Fees (in SEQ/Kb) smaller than this are considered zero fee for transaction creation (default: %s)"), FormatMoney(CWallet::minTxFee.GetFeePerK())) + "\n";
    strUsage += "  -paytxfee=<amt>        " + strprintf(_("Fee (in SEQ/kB) to add to transactions you send (default: %s)"), FormatMoney(payTxFee.GetFeePerK())) + "\n";
    strUsage += "  -rescan                " + _("Rescan the block chain for missing wallet transactions") + " " + _("on startup") + "\n";
    strUsage += "  -salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + " " + _("on startup") + "\n";
    strUsage += "  -sendfreetransactions  " + strprintf(_("Send transactions as zero-fee transactions if possible (default: %u)"), 0) + "\n";
    strUsage += "  -spendzeroconfchange   " + strprintf(_("Spend unconfirmed change when sending transactions (default: %u)"), 1) + "\n";
    strUsage += "  -txconfirmtarget=<n>   " + strprintf(_("If paytxfee is not set, include enough fee so transactions begin confirmation on average within n blocks (default: %u)"), 1) + "\n";
    strUsage += "  -maxtxfee=<amt>        " + strprintf(_("Maximum total fees to use in a single wallet transaction, setting too low may abort large transactions (default: %s)"), FormatMoney(maxTxFee)) + "\n";
    strUsage += "  -usehd                 " + _("Use hierarchical deterministic key generation (HD) after bip32. Only has effect during wallet creation/first start") + " " + strprintf(_("(default: %u)"), DEFAULT_USE_HD_WALLET);
    strUsage += "  -mnemonic              " + _("User defined mnemonic for HD wallet (bip39). Only has effect during wallet creation/first start (default: randomly generated)");
    strUsage += "  -mnemonicpassphrase    " + _("User defined memonic passphrase for HD wallet (bip39). Only has effect during wallet creation/first start (default: randomly generated)");
    strUsage += "  -hdseed                " + _("User defined seed for HD wallet (should be in hex). Only has effect during wallet creation/first start (default: randomly generated)");
    strUsage += "  -upgradewallet         " + _("Upgrade wallet to latest format") + " " + _("on startup") + "\n";
    strUsage += "  -wallet=<file>         " + _("Specify wallet file (within data directory)") + " " + strprintf(_("(default: %s)"), "wallet.dat") + "\n";
    strUsage += "  -walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n";
    strUsage += "  -zapwallettxes=<mode>  " + _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") + "\n";
    strUsage += "                         " + _("(1 = keep tx meta data e.g. account owner and payment request information, 2 = drop tx meta data)") + "\n";
    strUsage += "-createwalletbackups=<n> " + strprintf(_("Number of automatic wallet backups (default: %u)"), nWalletBackups);

#endif

    strUsage += "\n" + _("Debugging/Testing options:") + "\n";
    if (GetBoolArg("-help-debug", false))
    {
        strUsage += "  -checkpoints              " + strprintf("Disable expensive verification for known chain history (default: %u)", DEFAULT_CHECKPOINTS_ENABLED);
        strUsage += "  -dblogsize=<n>            " + strprintf(_("Flush database activity from memory pool to disk log every <n> megabytes (default: %u)"), 100) + "\n";
        strUsage += "  -disablesafemode          " + strprintf(_("Disable safemode, override a real safe mode event (default: %u)"), 0) + "\n";
        strUsage += "  -testsafemode             " + strprintf(_("Force safe mode (default: %u)"), 0) + "\n";
        strUsage += "  -dropmessagestest=<n>     " + _("Randomly drop 1 of every <n> network messages") + "\n";
        strUsage += "  -fuzzmessagestest=<n>     " + _("Randomly fuzz 1 of every <n> network messages") + "\n";
        strUsage += "  -flushwallet              " + strprintf(_("Run a thread to flush wallet periodically (default: %u)"), 1) + "\n";
        strUsage += "  -stopafterblockimport     " + strprintf(_("Stop running after importing blocks from disk (default: %u)"), 0) + "\n";
        strUsage += "  -limitancestorcount=<n>   " + strprintf("Do not accept transactions if number of in-mempool ancestors is <n> or more (default: %u)", DEFAULT_ANCESTOR_LIMIT);
        strUsage += "  -limitancestorsize=<n>    " + strprintf("Do not accept transactions whose size with all in-mempool ancestors exceeds <n> kilobytes (default: %u)", DEFAULT_ANCESTOR_SIZE_LIMIT);
        strUsage += "  -limitdescendantcount=<n> " + strprintf("Do not accept transactions if any ancestor would have <n> or more in-mempool descendants (default: %u)", DEFAULT_DESCENDANT_LIMIT);
        strUsage += "  -limitdescendantsize=<n>  " + strprintf("Do not accept transactions if any ancestor would have more than <n> kilobytes of in-mempool descendants (default: %u).", DEFAULT_DESCENDANT_SIZE_LIMIT);
    }
    strUsage += "  -debug=<category>      " + strprintf(_("Output debugging information (default: %u, supplying <category> is optional)"), 0) + "\n";
    strUsage += "                         " + _("If <category> is not supplied, output all debugging information.") + "\n";
    strUsage += "                         " + _("<category> can be:");
    strUsage +=                                 " addrman, alert, bench, coindb, db, lock, rand, rpc, selectcoins, mempool, net"; // Don't translate these and qt below
    if (mode == HMM_SEQUENCE_QT)
        strUsage += ", qt";
    strUsage += ".\n";
#ifdef ENABLE_WALLET
    strUsage += "  -gen                   " + strprintf(_("Generate coins (default: %u)"), 0) + "\n";
    strUsage += "  -genproclimit=<n>      " + strprintf(_("Set the number of threads for coin generation if enabled (-1 = all cores, default: %d)"), 1) + "\n";
#endif
    strUsage += "  -help-debug            " + _("Show all debugging options (usage: --help -help-debug)") + "\n";
    strUsage += "  -logips                " + strprintf(_("Include IP addresses in debug output (default: %u)"), 0) + "\n";
    strUsage += "  -logtimestamps         " + strprintf(_("Prepend debug output with timestamp (default: %u)"), 1) + "\n";
    if (GetBoolArg("-help-debug", false))
    {
        strUsage += "  -limitfreerelay=<n>    " + strprintf(_("Continuously rate-limit free transactions to <n>*1000 bytes per minute (default:%u)"), 15) + "\n";
        strUsage += "  -relaypriority         " + strprintf(_("Require high priority for relaying free or low-fee transactions (default:%u)"), 1) + "\n";
        strUsage += "  -maxsigcachesize=<n>   " + strprintf(_("Limit size of signature cache to <n> entries (default: %u)"), 50000) + "\n";
    }
    strUsage += "  -minrelaytxfee=<amt>   " + strprintf(_("Fees (in SEQ/Kb) smaller than this are considered zero fee for relaying (default: %s)"), FormatMoney(::minRelayTxFee.GetFeePerK())) + "\n";
    strUsage += "  -printtoconsole        " + _("Send trace/debug info to console instead of debug.log file") + "\n";
    if (GetBoolArg("-help-debug", false))
    {
        strUsage += "  -printpriority         " + strprintf(_("Log transaction priority and fee per kB when mining blocks (default: %u)"), 0) + "\n";
        strUsage += "  -privdb                " + strprintf(_("Sets the DB_PRIVATE flag in the wallet db environment (default: %u)"), 1) + "\n";
        strUsage += "  -regtest               " + _("Enter regression test mode, which uses a special chain in which blocks can be solved instantly.") + "\n";
        strUsage += "                         " + _("This is intended for regression testing tools and app development.") + "\n";
        strUsage += "                         " + _("In this mode -genproclimit controls how many blocks are generated immediately.") + "\n";
    }
    strUsage += "  -shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n";
    strUsage += "  -testnet               " + _("Use the test network") + "\n";

    strUsage += "\n" + _("Node relay options:") + "\n";
    strUsage += "  -datacarrier           " + strprintf(_("Relay and mine data carrier transactions (default: %u)"), 1) + "\n";
    strUsage += "  -datacarriersize       " + strprintf(_("Maximum size of data in data carrier transactions we relay and mine (default: %u)"), MAX_OP_RETURN_RELAY) + "\n";

    strUsage += "\n" + _("Block creation options:") + "\n";
    strUsage += "  -blockminsize=<n>      " + strprintf(_("Set minimum block size in bytes (default: %u)"), 0) + "\n";
    strUsage += "  -blockmaxsize=<n>      " + strprintf(_("Set maximum block size in bytes (default: %d)"), DEFAULT_BLOCK_MAX_SIZE) + "\n";
    strUsage += "  -blockprioritysize=<n> " + strprintf(_("Set maximum size of high-priority/low-fee transactions in bytes (default: %d)"), DEFAULT_BLOCK_PRIORITY_SIZE) + "\n";

    strUsage += "\n" + _("RPC server options:") + "\n";
    strUsage += "  -server                " + _("Accept command line and JSON-RPC commands") + "\n";
    strUsage += "  -rest                  " + strprintf(_("Accept public REST requests (default: %u)"), 0) + "\n";
    strUsage += "  -rpcbind=<addr>        " + _("Bind to given address to listen for JSON-RPC connections. Use [host]:port notation for IPv6. This option can be specified multiple times (default: bind to all interfaces)") + "\n";
    strUsage += "  -rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n";
    strUsage += "  -rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n";
    strUsage += "  -rpcport=<port>        " + strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or testnet: %u)"), BaseParams(CBaseChainParams::MAIN).RPCPort(), BaseParams(CBaseChainParams::TESTNET).RPCPort());
    strUsage += "  -rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified source. Valid for <ip> are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This option can be specified multiple times") + "\n";
    strUsage += "  -rpcthreads=<n>        " + strprintf(_("Set the number of threads to service RPC calls (default: %d)"), 4) + "\n";
    strUsage += "  -rpckeepalive          " + strprintf(_("RPC support for HTTP persistent connections (default: %d)"), 1) + "\n";

    strUsage += "\n" + _("RPC SSL options: (see the sequence Wiki for SSL setup instructions)") + "\n";
    strUsage += "  -rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n";
    strUsage += "  -rpcsslcertificatechainfile=<file.cert>  " + strprintf(_("Server certificate file (default: %s)"), "server.cert") + "\n";
    strUsage += "  -rpcsslprivatekeyfile=<file.pem>         " + strprintf(_("Server private key (default: %s)"), "server.pem") + "\n";
    strUsage += "  -rpcsslciphers=<ciphers>                 " + strprintf(_("Acceptable ciphers (default: %s)"), "TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH") + "\n";

    return strUsage;
}

std::string LicenseInfo()
{
    return FormatParagraph(strprintf(_("Copyright (c) 2016-%i Duality Blockchain Solutions Developers"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(_("Copyright (c) 2009-2017 Bitcoin Developers")) + "\n" +
           "\n" +
           FormatParagraph(_("Copyright (c) 2013-2017 Emercoin Developers")) + "\n" +
           "\n" +
           FormatParagraph(_("Copyright (c) 2011-2017 PPCoin Developers")) + "\n" +
           "\n" +
           FormatParagraph(_("Copyright (c) 2009-2017 Namecoin Developers")) + "\n" +
           "\n" +
           FormatParagraph(_("This is experimental software.")) + "\n" +
           "\n" +
           FormatParagraph(_("Distributed under the MIT software license, see the accompanying file COPYING or <http://www.opensource.org/licenses/mit-license.php>.")) + "\n" +
           "\n" +
           FormatParagraph(_("This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit <https://www.openssl.org/> and cryptographic software written by Eric Young and UPnP software written by Thomas Bernard.")) +
           "\n";
}

static void BlockNotifyCallback(bool initialSync, const CBlockIndex *pBlockIndex)
{
    if (initialSync || !pBlockIndex)
        return;

    std::string strCmd = GetArg("-blocknotify", "");

    boost::replace_all(strCmd, "%s", pBlockIndex->GetBlockHash().GetHex());
    boost::thread t(runCommand, strCmd); // thread runs free
}

struct CImportingNow
{
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};

void ThreadImport(std::vector<boost::filesystem::path> vImportFiles)
{
    const CChainParams& chainparams = Params();
    RenameThread("sequence-loadblk");

    // -reindex
    if (fReindex) {
        CImportingNow imp;
        int nFile = 0;
        while (true) {
            CDiskBlockPos pos(nFile, 0);
            if (!boost::filesystem::exists(GetBlockPosFilename(pos, "blk")))
                break; // No block files left to reindex
            FILE *file = OpenBlockFile(pos, true);
            if (!file)
                break; // This error is logged in OpenBlockFile
            LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
            LoadExternalBlockFile(chainparams, file, &pos);
            nFile++;
        }
        pblocktree->WriteReindexing(false);
        fReindex = false;
        LogPrintf("Reindexing finished\n");
        // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
        InitBlockIndex(chainparams);
    }

    // hardcoded $DATADIR/bootstrap.dat
    filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (filesystem::exists(pathBootstrap)) {
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LogPrintf("Importing bootstrap.dat...\n");
            LoadExternalBlockFile(chainparams, file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        } else {
            LogPrintf("Warning: Could not open bootstrap file %s\n", pathBootstrap.string());
        }
    }

    // -loadblock=
    for(boost::filesystem::path &path : vImportFiles) {
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            LogPrintf("Importing blocks file %s...\n", path.string());
            LoadExternalBlockFile(chainparams, file);
        } else {
            LogPrintf("Warning: Could not open blocks file %s\n", path.string());
        }
    }

    if (GetBoolArg("-stopafterblockimport", false)) {
        LogPrintf("Stopping after block import\n");
        StartShutdown();
    }
}

/** Sanity checks
 *  Ensure that sequence is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if(!ECC_InitSanityCheck()) {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.sequence.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    }
    if (!glibc_sanity_test()) {
	InitError("glibc_sanity_test() fails");
	return false;
    }

    if (!glibcxx_sanity_test()) {
	InitError("glibcxx_sanity_test() fails");
	return false;
    }

    return true;
}

/** Initialize sequence.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
    // We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
    // which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);

    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
    {
        return InitError(strprintf("Error: Winsock library failed to start (WSAStartup returned error %d)", ret));
    }
#endif
#ifndef WIN32

    if (GetBoolArg("-sysperms", false)) {
#ifdef ENABLE_WALLET
        if (!GetBoolArg("-disablewallet", false))
            return InitError("Error: -sysperms is not allowed in combination with enabled wallet functionality");
#endif
    } else {
        umask(077);
    }

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);

#if defined (__SVR4) && defined (__sun)
    // ignore SIGPIPE on Solaris
    signal(SIGPIPE, SIG_IGN);
#endif
#endif

    // ********************************************************* Step 2: parameter interactions
    const CChainParams& chainparams = Params();
    // Set this early so that parameter interactions go to console
    fPrintToConsole = GetBoolArg("-printtoconsole", false);
    fLogTimestamps = GetBoolArg("-logtimestamps", true);
    fLogIPs = GetBoolArg("-logips", false);

    if (mapArgs.count("-bind") || mapArgs.count("-whitebind")) {
        // when specifying an explicit binding address, you want to listen on it
        // even when -connect or -proxy is specified
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -listen=0\n", __func__);
        // to protect privacy, do not use UPNP when a proxy is set. The user may still specify -listen=1
        // to listen locally, so don't rely on this happening through -listen below.
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -upnp=0\n", __func__);
    }

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (SoftSetBoolArg("-dnsseed", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -dnsseed=0\n");
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -listen=0\n");
    }

    if (mapArgs.count("-proxy")) {
        // to protect privacy, do not listen by default if a default proxy server is specified
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -listen=0\n");
        // to protect privacy, do not discover addresses by default
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -discover=0\n");
    }

    if (!GetBoolArg("-listen", true)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -upnp=0\n");
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -discover=0\n");
        if (SoftSetBoolArg("-listenonion", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -listenonion=0\n", __func__);
    }

    if (mapArgs.count("-externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -externalip set -> setting -discover=0\n");
    }

    if (GetBoolArg("-salvagewallet", false)) {
        // Rewrite just private keys: rescan to find transactions
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -salvagewallet=1 -> setting -rescan=1\n");
    }

    // -zapwallettx implies a rescan
    if (GetBoolArg("-zapwallettxes", false)) {
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -zapwallettxes=<mode> -> setting -rescan=1\n");
    }

    // Forcing relay from whitelisted hosts implies we will accept relays from them in the first place.
    if (GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
        if (SoftSetBoolArg("-whitelistrelay", true))
            LogPrintf("%s: parameter interaction: -whitelistforcerelay=1 -> setting -whitelistrelay=1\n", __func__);
    }

    if (mapArgs.count("-hdseed") && IsHex(GetArg("-hdseed", "not hex")) && (mapArgs.count("-mnemonic") || mapArgs.count("-mnemonicpassphrase"))) {
        mapArgs.erase("-mnemonic");
        mapArgs.erase("-mnemonicpassphrase");
        LogPrintf("%s: parameter interaction: can't use -hdseed and -mnemonic/-mnemonicpassphrase together, will prefer -seed\n", __func__);
    }

    // Make sure enough file descriptors are available
    int nBind = std::max(
                (mapMultiArgs.count("-bind") ? mapMultiArgs.at("-bind").size() : 0) +
                (mapMultiArgs.count("-whitebind") ? mapMultiArgs.at("-whitebind").size() : 0), size_t(1));
    nMaxConnections = GetArg("-maxconnections", 125);
    nMaxConnections = std::max(std::min(nMaxConnections, (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS)), 0);
    int nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS);
    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));
    if (nFD - MIN_CORE_FILEDESCRIPTORS < nMaxConnections)
        nMaxConnections = nFD - MIN_CORE_FILEDESCRIPTORS;

    // ********************************************************* Step 3: parameter-to-internal-flags

    fDebug = !mapMultiArgs["-debug"].empty();
    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const vector<string>& categories = mapMultiArgs["-debug"];
    if (GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), string("0")) != categories.end())
        fDebug = false;

    // Check for -debugnet
    if (GetBoolArg("-debugnet", false))
        InitWarning(_("Warning: Unsupported argument -debugnet ignored, use -debug=net."));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("-socks"))
        return InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));
    // Check for -tor - as this is a privacy risk to continue, exit here
    if (GetBoolArg("-tor", false))
        return InitError(_("Error: Unsupported argument -tor found, use -onion."));

    if (GetBoolArg("-benchmark", false))
        InitWarning(_("Warning: Unsupported argument -benchmark ignored, use -debug=bench."));

    // Checkmempool and checkblockindex default to true in regtest mode
        mempool.setSanityCheck(GetBoolArg("-checkmempool", chainparams.DefaultConsistencyChecks()));
        fCheckBlockIndex = GetBoolArg("-checkblockindex", chainparams.DefaultConsistencyChecks());
        fCheckpointsEnabled = GetBoolArg("-checkpoints", DEFAULT_CHECKPOINTS_ENABLED);

    // -par=0 means autodetect, but nScriptCheckThreads==0 means no concurrency
    nScriptCheckThreads = GetArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (nScriptCheckThreads <= 0)
        nScriptCheckThreads += boost::thread::hardware_concurrency();
    if (nScriptCheckThreads <= 1)
        nScriptCheckThreads = 0;
    else if (nScriptCheckThreads > MAX_SCRIPTCHECK_THREADS)
        nScriptCheckThreads = MAX_SCRIPTCHECK_THREADS;

    fServer = GetBoolArg("-server", false);
    RegisterAllCoreRPCCommands(tableRPC);
#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("-disablewallet", false);
    if (!fDisableWallet)
        RegisterWalletRPCCommands(tableRPC);
#endif

    nConnectTimeout = GetArg("-timeout", DEFAULT_CONNECT_TIMEOUT);
    if (nConnectTimeout <= 0)
        nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

    // Fee-per-kilobyte amount considered the same as "free"
    // If you are mining, be careful setting this:
    // if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    if (mapArgs.count("-minrelaytxfee"))
    {
        CAmount n = 0;
        if (ParseMoney(mapArgs["-minrelaytxfee"], n) && n > 0)
            ::minRelayTxFee = CFeeRate(n);
        else
            return InitError(strprintf(_("Invalid amount for -minrelaytxfee=<amount>: '%s'"), mapArgs["-minrelaytxfee"]));
    }

    fRequireStandard = !GetBoolArg("-acceptnonstdtxn", !Params().RequireStandard());
    if (Params().RequireStandard() && !fRequireStandard)
        return InitError(strprintf("acceptnonstdtxn is not currently supported for %s chain", chainparams.NetworkIDString()));
    nBytesPerSigOp = GetArg("-bytespersigop", nBytesPerSigOp);

#ifdef ENABLE_WALLET
    if (mapArgs.count("-mintxfee"))
    {
        CAmount n = 0;
        if (ParseMoney(mapArgs["-mintxfee"], n) && n > 0)
            CWallet::minTxFee = CFeeRate(n);
        else
            return InitError(strprintf(_("Invalid amount for -mintxfee=<amount>: '%s'"), mapArgs["-mintxfee"]));
    }
    if (mapArgs.count("-paytxfee"))
    {
        CAmount nFeePerK = 0;
        if (!ParseMoney(mapArgs["-paytxfee"], nFeePerK))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"]));
        if (nFeePerK > nHighTransactionFeeWarning)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
        payTxFee = CFeeRate(nFeePerK, 1000);
        if (payTxFee < ::minRelayTxFee || payTxFee.GetFeePerK() < MIN_TX_FEE)
        {
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s' (must be at least %s)"),
                                       mapArgs["-paytxfee"], ::minRelayTxFee.ToString()));
        }
    }
    if (mapArgs.count("-maxtxfee"))
    {
        CAmount nMaxFee = 0;
        if (!ParseMoney(mapArgs["-maxtxfee"], nMaxFee))
            return InitError(strprintf(_("Invalid amount for -maxtxfee=<amount>: '%s'"), mapArgs["-maptxfee"]));
        if (nMaxFee > nHighTransactionMaxFeeWarning)
            InitWarning(_("Warning: -maxtxfee is set very high! Fees this large could be paid on a single transaction."));
        maxTxFee = nMaxFee;
        if (CFeeRate(maxTxFee, 1000) < ::minRelayTxFee)
        {
            return InitError(strprintf(_("Invalid amount for -maxtxfee=<amount>: '%s' (must be at least the minrelay fee of %s to prevent stuck transactions)"),
                                       mapArgs["-maxtxfee"], ::minRelayTxFee.ToString()));
        }
    }
    if (mapArgs.count("-reservebalance"))
    {
        CAmount nReserveBalance = 0;
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
            return InitError(strprintf(_("Invalid amount for -reservebalance=<amount>: '%s'"), mapArgs["-reservebalance"]));
    }
    if (mapArgs.count("-checkpointkey")) // ppcoin: checkpoint master priv key
    {
        if (!CheckpointsSync::SetCheckpointPrivKey(GetArg("-checkpointkey", "")))
            return InitError(_("Unable to sign checkpoint, wrong checkpointkey?\n"));
        else LogPrintf("Setting checkpoint private key is successful\n");
    }
    nTxConfirmTarget = GetArg("-txconfirmtarget", 1);
    bSpendZeroConfChange = GetArg("-spendzeroconfchange", true);
    fSendFreeTransactions = GetArg("-sendfreetransactions", false);

    std::string strWalletFile = GetArg("-wallet", "wallet.dat");
#endif // ENABLE_WALLET

    fIsBareMultisigStd = GetArg("-permitbaremultisig", true) != 0;
    nMaxDatacarrierBytes = GetArg("-datacarriersize", nMaxDatacarrierBytes);

    fAlerts = GetBoolArg("-alerts", DEFAULT_ALERTS);

    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log

    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. Sequence is shutting down."));

    std::string strDataDir = GetDataDir().string();
#ifdef ENABLE_WALLET
    // Wallet file must be a plain filename without a directory
    if (strWalletFile != boost::filesystem::basename(strWalletFile) + boost::filesystem::extension(strWalletFile))
        return InitError(strprintf(_("Wallet %s resides outside data directory %s"), strWalletFile, strDataDir));
#endif
    // Make sure only a single sequence process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);
    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
    if (!lock.try_lock())
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. Sequence is probably already running."), strDataDir));
#ifndef WIN32
    CreatePidFile(GetPidFile(), getpid());
#endif
    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();
    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Sequence version %s\n", FormatFullVersion());
#ifdef ENABLE_WALLET
    LogPrintf("Using BerkeleyDB version %s\n", DbEnv::version(0, 0, 0));
#endif
    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()));
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Using data directory %s\n", strDataDir);
    LogPrintf("Using config file %s\n", GetConfigFile().string());
    LogPrintf("Using at most %i connections (%i file descriptors available)\n", nMaxConnections, nFD);
    std::ostringstream strErrors;

    LogPrintf("Using %u threads for script verification\n", nScriptCheckThreads);
    if (nScriptCheckThreads) {
        for (int i=0; i<nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
    }


    // Start the lightweight task scheduler thread (TODO: Fix assertion failure)
    //CScheduler::Function serviceLoop = boost::bind(&CScheduler::serviceQueue, &scheduler);
    //threadGroup.create_thread(boost::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

    /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    if (fServer)
    {
        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        StartRPCThreads();
    }

    // do not use hooks below this line
    // hooks = InitHook();

    int64_t nStart;

    // ********************************************************* Step 5: verify wallet database integrity
#ifdef ENABLE_WALLET
    if (!fDisableWallet) {
        std::string strWarning;
        std::string strError;

        nWalletBackups = GetArg("-createwalletbackups", 10);
        nWalletBackups = std::max(0, std::min(10, nWalletBackups));

        if(!AutoBackupWallet(NULL, strWalletFile, strWarning, strError)) {
            if (!strWarning.empty())
                InitWarning(strWarning);
            if (!strError.empty())
                return InitError(strError);
        }

        LogPrintf("Using wallet %s\n", strWalletFile);
        uiInterface.InitMessage(_("Verifying wallet..."));

        // reset warning string
        strWarning = "";

        if (!bitdb.Open(GetDataDir()))
        {
            // try moving the database env out of the way
            boost::filesystem::path pathDatabase = GetDataDir() / "database";
            boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
            try {
                boost::filesystem::rename(pathDatabase, pathDatabaseBak);
                LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
            } catch(boost::filesystem::filesystem_error &error) {
                 // failure is ok (well, not really, but it's not worse than what we started with)
            }

            // try again
            if (!bitdb.Open(GetDataDir())) {
                // if it still fails, it probably means we can't even create the database env
                string msg = strprintf(_("Error initializing wallet database environment %s!"), strDataDir);
                return InitError(msg);
            }
        }

        if (GetBoolArg("-salvagewallet", false))
        {
            // Recover readable keypairs:
            if (!CWalletDB::Recover(bitdb, strWalletFile, true))
                return false;
        }

        if (filesystem::exists(GetDataDir() / strWalletFile))
        {
            CDBEnv::VerifyResult r = bitdb.Verify(strWalletFile, CWalletDB::Recover);
            if (r == CDBEnv::RECOVER_OK)
            {
                string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                         " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                         " your balance or transactions are incorrect you should"
                                         " restore from a backup."), strDataDir);
                InitWarning(msg);
            }
            if (r == CDBEnv::RECOVER_FAIL)
                return InitError(_("wallet.dat corrupt, salvage failed"));
        }
    } // (!fDisableWallet)
#endif // ENABLE_WALLET
    // ********************************************************* Step 6: network initialization

    RegisterNodeSignals(GetNodeSignals());

    // sanitize comments per BIP-0014, format user agent and check total size
    std::vector<string> uacomments;
    for(string cmt : mapMultiArgs["-uacomment"])
    {
        if (cmt != SanitizeString(cmt, SAFE_CHARS_UA_COMMENT))
            return InitError(strprintf(_("User Agent comment (%s) contains unsafe characters."), cmt));
        uacomments.push_back(SanitizeString(cmt, SAFE_CHARS_UA_COMMENT));
    }
    strSubVersion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, uacomments);
    if (strSubVersion.size() > MAX_SUBVERSION_LENGTH) {
        return InitError(strprintf(_("Total length of network version string (%i) exceeds maximum length (%i). Reduce the number or size of uacomments."),
            strSubVersion.size(), MAX_SUBVERSION_LENGTH));
    }

    if (mapArgs.count("-onlynet")) {
        std::set<enum Network> nets;
        for(std::string snet : mapMultiArgs["-onlynet"]) {
            enum Network net = ParseNetwork(snet);
            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));
            nets.insert(net);
        }
        for (int n = 0; n < NET_MAX; n++) {
            enum Network net = (enum Network)n;
            if (!nets.count(net))
                SetLimited(net);
        }
    }

    if (mapArgs.count("-whitelist")) {
        for(const std::string& net : mapMultiArgs["-whitelist"]) {
            CSubNet subnet(net);
            if (!subnet.IsValid())
                return InitError(strprintf(_("Invalid netmask specified in -whitelist: '%s'"), net));
            CNode::AddWhitelistedRange(subnet);
        }
    }

    bool proxyRandomize = GetBoolArg("-proxyrandomize", DEFAULT_PROXYRANDOMIZE);
    // -proxy sets a proxy for all outgoing network traffic
    // -noproxy (or -proxy=0) as well as the empty string can be used to not set a proxy, this is the default
    std::string proxyArg = GetArg("-proxy", "");
    SetLimited(NET_TOR);
    if (proxyArg != "" && proxyArg != "0") {
        proxyType addrProxy = proxyType(CService(proxyArg, 9050), proxyRandomize);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), proxyArg));

        SetProxy(NET_IPV4, addrProxy);
        SetProxy(NET_IPV6, addrProxy);
        SetProxy(NET_TOR, addrProxy);
        SetNameProxy(addrProxy);
        SetLimited(NET_TOR, false); // by default, -proxy sets onion as reachable, unless -noonion later
    }

    // -onion can be used to set only a proxy for .onion, or override normal proxy for .onion addresses
    // -noonion (or -onion=0) disables connecting to .onion entirely
    // An empty string is used to not override the onion proxy (in which case it defaults to -proxy set above, or none)
    std::string onionArg = GetArg("-onion", "");
    if (onionArg != "") {
        if (onionArg == "0") { // Handle -noonion/-onion=0
            SetLimited(NET_TOR); // set onions as unreachable
        } else {
            proxyType addrOnion = proxyType(CService(onionArg, 9050), proxyRandomize);
            if (!addrOnion.IsValid())
                return InitError(strprintf(_("Invalid -onion address: '%s'"), onionArg));
            SetProxy(NET_TOR, addrOnion);
            SetLimited(NET_TOR, false);
        }
    }

    // see Step 2: parameter interactions for more information about these
    fListen = GetBoolArg("-listen", DEFAULT_LISTEN);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);

    bool fBound = false;
    if (fListen) {
        if (mapArgs.count("-bind") || mapArgs.count("-whitebind")) {
            for(std::string strBind : mapMultiArgs["-bind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind));
                fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR));
            }
            for(std::string strBind : mapMultiArgs["-whitebind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, 0, false))
                    return InitError(strprintf(_("Cannot resolve -whitebind address: '%s'"), strBind));
                if (addrBind.GetPort() == 0)
                    return InitError(strprintf(_("Need to specify a port with -whitebind: '%s'"), strBind));
                fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR | BF_WHITELIST));
            }
        }
        else {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            fBound |= Bind(CService(in6addr_any, GetListenPort()), BF_NONE);
            fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound ? BF_REPORT_ERROR : BF_NONE);
        }
        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    }

    if (mapArgs.count("-externalip")) {
        for(string strAddr : mapMultiArgs["-externalip"]) {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);
            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr));
            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        }
    }

    for(string strDest : mapMultiArgs["-seednode"])
        AddOneShot(strDest);

    // ********************************************************* Step 7: load block chain

    fReindex = GetBoolArg("-reindex", false);

    // Upgrading to 0.8; hard-link the old blknnnn.dat files into /blocks/
    filesystem::path blocksDir = GetDataDir() / "blocks";
    if (!filesystem::exists(blocksDir))
    {
        filesystem::create_directories(blocksDir);
        bool linked = false;
        for (unsigned int i = 1; i < 10000; i++) {
            filesystem::path source = GetDataDir() / strprintf("blk%04u.dat", i);
            if (!filesystem::exists(source)) break;
            filesystem::path dest = blocksDir / strprintf("blk%05u.dat", i-1);
            try {
                filesystem::create_hard_link(source, dest);
                LogPrintf("Hardlinked %s -> %s\n", source.string(), dest.string());
                linked = true;
            } catch (filesystem::filesystem_error & e) {
                // Note: hardlink creation failing is not a disaster, it just means
                // blocks will get re-downloaded from peers.
                LogPrintf("Error hardlinking blk%04u.dat : %s\n", i, e.what());
                break;
            }
        }
        if (linked)
        {
            fReindex = true;
        }
    }

    // cache size calculations
    int64_t nTotalCache = (GetArg("-dbcache", nDefaultDbCache) << 20);
    nTotalCache = std::max(nTotalCache, nMinDbCache << 20); // total cache cannot be less than nMinDbCache
    nTotalCache = std::min(nTotalCache, nMaxDbCache << 20); // total cache cannot be greated than nMaxDbcache
    int64_t nBlockTreeDBCache = nTotalCache / 8;
    if (nBlockTreeDBCache > (1 << 21) && !GetBoolArg("-txindex", true))
        nBlockTreeDBCache = (1 << 21); // block tree db cache shouldn't be larger than 2 MiB
    nTotalCache -= nBlockTreeDBCache;
    int64_t nCoinDBCache = std::min(nTotalCache / 2, (nTotalCache / 4) + (1 << 23)); // use 25%-50% of the remainder for disk cache
    nTotalCache -= nCoinDBCache;
    nCoinCacheUsage = nTotalCache; // the rest goes to in-memory cache
    LogPrintf("Cache configuration:\n");
    LogPrintf("* Using %.1fMiB for block index database\n", nBlockTreeDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for chain state database\n", nCoinDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for in-memory UTXO set\n", nCoinCacheUsage * (1.0 / 1024 / 1024));

    bool fLoaded = false;
    while (!fLoaded) {
        bool fReset = fReindex;
        std::string strLoadError;

        uiInterface.InitMessage(_("Loading block index..."));

        nStart = GetTimeMillis();
        do {
            try {
                UnloadBlockIndex();
                delete pcoinsTip;
                delete pcoinsdbview;
                delete pcoinscatcher;
                delete pblocktree;

                if (fReindex)
                {
                    boost::system::error_code err;
                    filesystem::remove(GetDataDir() / "ddns.dat", err);
                }
                pblocktree = new CBlockTreeDB(nBlockTreeDBCache, false, fReindex);
                pcoinsdbview = new CCoinsViewDB(nCoinDBCache, false, fReindex);
                pcoinscatcher = new CCoinsViewErrorCatcher(pcoinsdbview);
                pcoinsTip = new CCoinsViewCache(pcoinscatcher);

                if (fReindex)
                    pblocktree->WriteReindexing(true);

                if (!LoadBlockIndex()) {
                    strLoadError = _("Error loading block database");
                    break;
                }

                // If the loaded chain has a wrong genesis, bail out immediately
                // (we're likely using a testnet datadir, or the other way around).
                if (!mapBlockIndex.empty() && mapBlockIndex.count(chainparams.GetConsensus().hashGenesisBlock) == 0)
                    return InitError(_("Incorrect or no genesis block found. Wrong datadir for network?"));

                // Initialize the block index (no-op if non-empty database was already loaded)
                if (!InitBlockIndex(chainparams)) {
                    strLoadError = _("Error initializing block database");
                    break;
                }

                // Check for changed -txindex state
                if (fTxIndex != GetBoolArg("-txindex", true)) {
                    strLoadError = _("You need to rebuild the database using -reindex to change -txindex");
                    break;
                }

                uiInterface.InitMessage(_("Verifying blocks..."));
                if (!CVerifyDB().VerifyDB(pcoinsdbview, GetArg("-checklevel", 3),
                              GetArg("-checkblocks", 288))) {
                    strLoadError = _("Corrupted block database detected");
                    break;
                }
            } catch(const std::exception& e) {
                if (fDebug) LogPrintf("%s\n", e.what());
                strLoadError = _("Error opening block database");
                break;
            }
            fLoaded = true;
        } while(false);

        if (!fLoaded) {
            // first suggest a reindex
            if (!fReset) {
                bool fRet = uiInterface.ThreadSafeMessageBox(
                    strLoadError + ".\n\n" + _("Do you want to rebuild the block database now?"),
                    "", CClientUIInterface::MSG_ERROR | CClientUIInterface::BTN_ABORT);
                if (fRet) {
                    fReindex = true;
                    fRequestShutdown = false;
                } else {
                    LogPrintf("Aborted block database rebuild. Exiting.\n");
                    return false;
                }
            } else {
                return InitError(strLoadError);
            }
        }
    }

    // As LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }
    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    // Sequence: check in nameindex need to be created or recreated
    // we should have block index fully loaded by now
    extern bool createNameIndexFile();
    if (!filesystem::exists(GetDataDir() / "ddns.dat") && !createNameIndexFile())
    {
        LogPrintf("Fatal error: Failed to create nameindex.\n");
        return false;
    }

    boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
    CAutoFile est_filein(fopen(est_path.string().c_str(), "rb"), SER_DISK, CLIENT_VERSION);
    // Allowed to fail as this file IS missing on first startup.
    if (!est_filein.IsNull())
        mempool.ReadFeeEstimates(est_filein);
    fFeeEstimatesInitialized = true;

    // ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
    if (fDisableWallet) {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    } else {

        // needed to restore wallet transaction meta data after -zapwallettxes
        std::vector<CWalletTx> vWtx;

        if (GetBoolArg("-zapwallettxes", false)) {
            uiInterface.InitMessage(_("Zapping all transactions from wallet..."));

            pwalletMain = new CWallet(strWalletFile);
            DBErrors nZapWalletRet = pwalletMain->ZapWalletTx(vWtx);
            if (nZapWalletRet != DB_LOAD_OK) {
                uiInterface.InitMessage(_("Error loading wallet.dat: Wallet corrupted"));
                return false;
            }

            delete pwalletMain;
            pwalletMain = NULL;
        }

        uiInterface.InitMessage(_("Loading wallet..."));

        nStart = GetTimeMillis();
        bool fFirstRun = true;
        pwalletMain = new CWallet(strWalletFile);
        DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
        
        if (nLoadWalletRet != DB_LOAD_OK)
        {
            if (nLoadWalletRet == DB_CORRUPT)
                strErrors << _("Error loading wallet.dat: Wallet is either corrupted OR you need to use your previous client to dump ALL private keys for ALL addresses."
                               " BACKUP your backups folder and wallet.dat to a safe place i.e. USB storage or Paper Wallet."
                               " Allow Sequence to build a new wallet.dat, fully sync and then import the private keys to the newly created wallet") << "\n";
            else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
            {
                string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                             " or address book entries might be missing or incorrect."));
                InitWarning(msg);
            }
            else if (nLoadWalletRet == DB_TOO_NEW)
                strErrors << _("Error loading wallet.dat: Wallet requires newer version of Sequence") << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE)
            {
                strErrors << _("Wallet needed to be rewritten: restart Sequence to complete") << "\n";
                LogPrintf("%s", strErrors.str());
                return InitError(strErrors.str());
            }
            else
                strErrors << _("Error loading wallet.dat") << "\n";
        }

        if (GetBoolArg("-upgradewallet", fFirstRun))
        {
            int nMaxVersion = GetArg("-upgradewallet", 0);
            if (nMaxVersion == 0) // the -upgradewallet without argument case
            {
                LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
                nMaxVersion = CLIENT_VERSION;
                pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
            }
            else
                LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);
            if (nMaxVersion < pwalletMain->GetVersion())
                strErrors << _("Cannot downgrade wallet") << "\n";
            pwalletMain->SetMaxVersion(nMaxVersion);
        }

        if (fFirstRun)
        {
            // Create new keyUser and set as default key
            RandAddSeedPerfmon();

            if (GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET) && !pwalletMain->IsHDEnabled()) {
                if (GetArg("-mnemonicpassphrase", "").size() > 256)
                    return InitError(_("Mnemonic passphrase is too long, must be at most 256 characters"));
                // generate a new master key
                pwalletMain->GenerateNewHDChain();
            }

            CPubKey newDefaultKey;
            if (pwalletMain->GetKeyFromPool(newDefaultKey, false)) {
                pwalletMain->SetDefaultKey(newDefaultKey);
                if (!pwalletMain->SetAddressBook(pwalletMain->vchDefaultKey.GetID(), "", "receive"))
                    strErrors << _("Cannot write default address") << "\n";
            }

            pwalletMain->SetBestChain(chainActive.GetLocator());
        }
        else if (mapArgs.count("-usehd")) {
           bool useHD = GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET);
            if (pwalletMain->IsHDEnabled() && !useHD)
                return InitError(strprintf(_("Error loading %s: You can't disable HD on a already existing HD wallet"), strWalletFile));
            if (!pwalletMain->IsHDEnabled() && useHD)
                return InitError(strprintf(_("Error loading %s: You can't enable HD on a already existing non-HD wallet"), strWalletFile));
        }

        // Warn user every time he starts non-encrypted HD wallet
        if (GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET) && !pwalletMain->IsLocked()) {
            InitWarning(_("Make sure to encrypt your wallet and delete all non-encrypted backups after you verified that wallet works!"));
        }

        LogPrintf("%s", strErrors.str());
        LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

        RegisterValidationInterface(pwalletMain);

        CBlockIndex *pindexRescan = chainActive.Tip();
        if (GetBoolArg("-rescan", false))
            pindexRescan = chainActive.Genesis();
        else
        {
            CWalletDB walletdb(strWalletFile);
            CBlockLocator locator;
            if (walletdb.ReadBestBlock(locator))
                pindexRescan = FindForkInGlobalIndex(chainActive, locator);
            else
                pindexRescan = chainActive.Genesis();
        }
        if (chainActive.Tip() && chainActive.Tip() != pindexRescan)
        {
            uiInterface.InitMessage(_("Rescanning..."));
            LogPrintf("Rescanning last %i blocks (from block %i)...\n", chainActive.Height() - pindexRescan->nHeight, pindexRescan->nHeight);
            nStart = GetTimeMillis();
            pwalletMain->ScanForWalletTransactions(pindexRescan, true);
            LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
            pwalletMain->SetBestChain(chainActive.GetLocator());
            nWalletDBUpdated++;

            // Restore wallet transaction metadata after -zapwallettxes=1
            if (GetBoolArg("-zapwallettxes", false) && GetArg("-zapwallettxes", "1") != "2")
            {
                CWalletDB walletdb(strWalletFile);

                for(const CWalletTx& wtxOld : vWtx)
                {
                    uint256 hash = wtxOld.GetHash();
                    std::map<uint256, CWalletTx>::iterator mi = pwalletMain->mapWallet.find(hash);
                    if (mi != pwalletMain->mapWallet.end())
                    {
                        const CWalletTx* copyFrom = &wtxOld;
                        CWalletTx* copyTo = &mi->second;
                        copyTo->mapValue = copyFrom->mapValue;
                        copyTo->vOrderForm = copyFrom->vOrderForm;
                        copyTo->nTimeReceived = copyFrom->nTimeReceived;
                        copyTo->nTimeSmart = copyFrom->nTimeSmart;
                        copyTo->fFromMe = copyFrom->fFromMe;
                        copyTo->strFromAccount = copyFrom->strFromAccount;
                        copyTo->nOrderPos = copyFrom->nOrderPos;
                        copyTo->WriteToDisk();
                    }
                }
            }
        }
    } // (!fDisableWallet)
#else // ENABLE_WALLET
    LogPrintf("No wallet compiled in!\n");
#endif // !ENABLE_WALLET
    // ********************************************************* Step 9: import blocks

    if (mapArgs.count("-blocknotify"))
        uiInterface.NotifyBlockTip.connect(BlockNotifyCallback);

    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("-loadblock"))
    {
        for(string strFile : mapMultiArgs["-loadblock"])
            vImportFiles.push_back(strFile);
    }
    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));
    if (chainActive.Tip() == NULL) {
        LogPrintf("Waiting for genesis block to be imported...\n");
        while (!fRequestShutdown && chainActive.Tip() == NULL)
            MilliSleep(10);
    }

    // ********************************************************* Step 10: start node

    if (!CheckDiskSpace())
        return false;

    if (!strErrors.str().empty())
        return InitError(strErrors.str());

    RandAddSeedPerfmon();

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n",   mapBlockIndex.size());
    LogPrintf("chainActive.Height() = %d\n",                   chainActive.Height());
#ifdef ENABLE_WALLET
    if (pwalletMain) {
        LOCK(pwalletMain->cs_wallet);
        LogPrintf("setExternalKeyPool.size() = %u\n",   pwalletMain->KeypoolCountExternalKeys());
        LogPrintf("setInternalKeyPool.size() = %u\n",   pwalletMain->KeypoolCountInternalKeys());
        LogPrintf("mapWallet.size() = %u\n",            pwalletMain->mapWallet.size());
        LogPrintf("mapAddressBook.size() = %u\n",       pwalletMain->mapAddressBook.size());
    } else {
        LogPrintf("wallet is NULL\n");
    }
#endif
    if (GetBoolArg("-listenonion", DEFAULT_LISTEN_ONION))
        StartTorControl(threadGroup, scheduler);


    StartNode(threadGroup, scheduler);

#ifdef ENABLE_WALLET
    // InitRPCMining is needed here so getwork/getblocktemplate in the GUI debug console works properly.       
    InitRPCMining();
    // Generate coins in the background
    if (pwalletMain)
        GenerateSequences(GetBoolArg("-gen", DEFAULT_GENERATE), pwalletMain, GetArg("-genproclimit", DEFAULT_GENERATE_THREADS), chainparams);
    if (GetBoolArg("-stakegen", true) && !(GetBoolArg("-disablewallet", false)))
        MintStake(threadGroup, pwalletMain);
#endif

    // ********************************************************* Step 11: NTP synchronization      
      
    // Trusted NTP server, it's localhost by default.     
    strTrustedUpstream = GetArg("-ntp", "localhost");

    // ********************************************************* Step 12: finished

    SetRPCWarmupFinished();
    uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
    if (pwalletMain) {
        // Add wallet transactions that aren't already in a block to mapTransactions
        pwalletMain->ReacceptWalletTransactions();

        // Run a thread to flush wallet periodically
        threadGroup.create_thread(boost::bind(&ThreadFlushWalletDB, boost::ref(pwalletMain->strWalletFile)));
    }
#endif

    // init seqdns. WARNING: this should be done after hooks initialization
    if (GetBoolArg("-seqdns", false))
    {
        #define SEQDNS_PORT 5335
        int port = GetArg("-seqdnsport", SEQDNS_PORT);
        int verbose = GetArg("-seqdnsverbose", 1);
        if (port <= 0)
            port = SEQDNS_PORT;
        string suffix  = GetArg("-seqdnssuffix", "");
        string bind_ip = GetArg("-seqdnsbindip", "");
        string allowed = GetArg("-seqdnsallowed", "");
        string localcf = GetArg("-seqdnslocalcf", "");
        string enums   = GetArg("-enumtrust", "");
        string tf      = GetArg("-enumtollfree", "");
        seqdns = new SeqDns(bind_ip.c_str(), port,
        suffix.c_str(), allowed.c_str(), localcf.c_str(), enums.c_str(), tf.c_str(), verbose);
        LogPrintf("dDNS server started\n");
    }

    return !fRequestShutdown;
}
