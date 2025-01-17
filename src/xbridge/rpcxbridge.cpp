// Copyright (c) 2017-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <xbridge/util/settings.h>
#include <xbridge/util/logger.h>
#include <xbridge/util/xbridgeerror.h>
#include <xbridge/util/xseries.h>
#include <xbridge/util/xutil.h>
#include <xbridge/xbridgeapp.h>
#include <xbridge/xbridgeexchange.h>
#include <xbridge/xbridgetransaction.h>
#include <xbridge/xbridgetransactiondescr.h>
#include <xbridge/xuiconnector.h>

#include <init.h>
#include <rpc/util.h>
#include <shutdown.h>
#include <validation.h>

#include <array>
#include <atomic>
#include <math.h>
#include <numeric>
#include <stdio.h>

#include <json/json_spirit_reader_template.h>
#include <json/json_spirit_writer_template.h>
#include <json/json_spirit_utils.h>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/lexical_cast.hpp>

using namespace json_spirit;
using namespace std;
using namespace boost;

using TransactionMap    = std::map<uint256, xbridge::TransactionDescrPtr>;
using TransactionPair   = std::pair<uint256, xbridge::TransactionDescrPtr>;
using RealVector        = std::vector<double>;
using TransactionVector = std::vector<xbridge::TransactionDescrPtr>;
using ArrayValue        = Array::value_type;
using ArrayIL           = std::initializer_list<ArrayValue>;

UniValue uret(const json_spirit::Value & o) {
    UniValue uv;
    if (!uv.read(json_spirit::write_string(o, json_spirit::none, 8)))
        throw runtime_error("Unknown server error: failed to process request");
    return uv;
}

/**
 * @brief TxOutToCurrencyPair inspects a CTxOut and returns currency pair transaction info
 * @param tx.vout - transaction outpoints with possible multisig/op_return
 * @param snode_pubkey - (output) the service node public key
 * @return - currency pair transaction details
 */
CurrencyPair TxOutToCurrencyPair(const std::vector<CTxOut> & vout, std::string& snode_pubkey)
{
    snode_pubkey.clear();

    if (vout.empty())
        return {};

    bool foundOpData{false};
    std::string json;

    for (const CTxOut & out : vout) {
        if (out.scriptPubKey.empty())
            continue;

        std::vector<std::vector<unsigned char> > solutions;
        txnouttype type = Solver(out.scriptPubKey, solutions);

        if (type == TX_MULTISIG) {
            if (solutions.size() < 4)
                continue;

            snode_pubkey = EncodeDestination(CTxDestination(CPubKey(solutions[1]).GetID()));
            for (size_t i = 2; i < solutions.size()-1; ++i) {
                const auto& sol = solutions[i];
                if (sol.size() != 65)
                    break;
                std::copy(sol.begin()+1, sol.end(), std::back_inserter(json));
            }
        } else if (type == TX_NULL_DATA) {
            if (out.nValue != 0 || !out.scriptPubKey.IsUnspendable())
                continue;
            std::vector<unsigned char> data;
            CScript::const_iterator pc = out.scriptPubKey.begin();
            while (pc < out.scriptPubKey.end()) { // look for order data
                opcodetype opcode;
                if (!out.scriptPubKey.GetOp(pc, opcode, data))
                    break;
                if (data.size() != 0) {
                    std::copy(data.begin(), data.end(), std::back_inserter(json));
                    foundOpData = true;
                    break;
                }
            }
        }
    }

    if (json.empty())
        return {}; // no data found

    if (foundOpData && vout.size() >= 2) {
        CTxDestination snodeAddr;
        if (ExtractDestination(vout[1].scriptPubKey, snodeAddr))
            snode_pubkey = EncodeDestination(snodeAddr);
    }

    json_spirit::Value val;
    if (not json_spirit::read_string(json, val) || val.type() != json_spirit::array_type)
        return {"unknown chain data, json error"};
    json_spirit::Array xtx = val.get_array();
    if (xtx.size() != 5)
        return {"unknown chain data, bad records count"};
    // validate chain inputs
    try { xtx[0].get_str(); } catch(...) {
        return {"bad id" }; }
    try { xtx[1].get_str(); } catch(...) {
        return {"bad from currency" }; }
    try { xtx[2].get_uint64(); } catch(...) {
        return {"bad from amount" }; }
    try { xtx[3].get_str(); } catch(...) {
        return {"bad to currency" }; }
    try { xtx[4].get_uint64(); } catch(...) {
        return {"bad to amount" }; }

    return CurrencyPair{
            xtx[0].get_str(),    // xid
            {ccy::Currency{xtx[1].get_str(),xbridge::TransactionDescr::COIN}, // fromCurrency
             xtx[2].get_uint64()},                                     // fromAmount
            {ccy::Currency{xtx[3].get_str(),xbridge::TransactionDescr::COIN}, // toCurrency
             xtx[4].get_uint64()}                                      // toAmount
    };
}

UniValue dxGetNewTokenAddress(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetNewTokenAddress",
                "\nget new address\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetNewTokenAddress", "BTC")
                  + HelpExampleRpc("dxGetNewTokenAddress", "BTC")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() != 1)
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, "(ticker)"));

    const auto currency = params[0].get_str();
    Array res;

    xbridge::WalletConnectorPtr conn = xbridge::App::instance().connectorByCurrency(currency);

    if (conn) {
        const auto addr = conn->getNewTokenAddress();
        if (!addr.empty())
            res.emplace_back(addr);
    }

    return uret(res);
}

UniValue dxLoadXBridgeConf(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxLoadXBridgeConf",
                "\nHot loads xbridge.conf (note this may disrupt trades in progress)\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxLoadXBridgeConf", "")
                  + HelpExampleRpc("dxLoadXBridgeConf", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() > 0)
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "This function does not accept any parameter"));

    if (ShutdownRequested())
        throw runtime_error("dxLoadXBridgeConf\nFailed to reload the config because a shutdown request is in progress.");

    auto & app = xbridge::App::instance();
    if (app.isUpdatingWallets()) // let the user know if wallets are being actively updated
        throw runtime_error("dxLoadXBridgeConf\nAn existing wallet update is currently in progress, please wait until it has completed.");

    auto success = app.loadSettings();
    app.clearBadWallets(); // clear any bad wallet designations b/c user is explicitly requesting a wallet update
    app.updateActiveWallets();
    return uret(success);
}

UniValue dxGetLocalTokens(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetLocalTokens",
                "\nList coins supported by your node. You can only trade with these supported coins.\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetLocalTokens", "")
                  + HelpExampleRpc("dxGetLocalTokens", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() > 0) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "This function does not accept any parameter"));

    }

    Array r;

    std::vector<std::string> currencies = xbridge::App::instance().availableCurrencies();
    for (std::string currency : currencies) {

        r.emplace_back(currency);

    }
    return uret(r);
}

UniValue dxGetNetworkTokens(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetNetworkTokens",
                "\nList coins supported by the network.\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetNetworkTokens", "")
                  + HelpExampleRpc("dxGetNetworkTokens", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() > 0) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "This function does not accept any parameters"));

    }

    std::set<std::string> services;
    auto ws = xbridge::App::instance().walletServices();
    for (auto & serviceItem : ws) {
        auto s = serviceItem.second.services();
        services.insert(s.begin(), s.end());
    }

    return uret(Array{services.begin(), services.end()});
}

/** \brief Returns the list of open and pending transactions
  * \param params A list of input params.
  * \param request.fHelp For debug purposes, throw the exception describing parameters.
  * \return A list of open(they go first) and pending transactions.
  *
  * Returns the list of open and pending transactions as JSON structures.
  * The open transactions go first.
  */
UniValue dxGetOrders(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetOrders",
                "\nList of all orders.\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetOrders", "")
                  + HelpExampleRpc("dxGetOrders", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (!params.empty()) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "This function does not accept any parameters"));

    }

    auto &xapp = xbridge::App::instance();
    TransactionMap trlist = xapp.transactions();
    auto currentTime = boost::posix_time::second_clock::universal_time();

    Array result;
    for (const auto& trEntry : trlist) {

        const auto &tr = trEntry.second;

        // Skip canceled, finished, and expired orders older than 1 minute
        if ((currentTime - tr->txtime).total_seconds() > 60) {
            if (tr->state == xbridge::TransactionDescr::trCancelled
              || tr->state == xbridge::TransactionDescr::trFinished
              || tr->state == xbridge::TransactionDescr::trExpired)
            continue;
        }

        xbridge::WalletConnectorPtr connFrom = xapp.connectorByCurrency(tr->fromCurrency);
        xbridge::WalletConnectorPtr connTo   = xapp.connectorByCurrency(tr->toCurrency);
        if (!connFrom || !connTo) {

            continue;

        }

        Object jtr;
        jtr.emplace_back(Pair("id",             tr->id.GetHex()));
        jtr.emplace_back(Pair("maker",          tr->fromCurrency));
        jtr.emplace_back(Pair("maker_size",     xbridge::xBridgeStringValueFromAmount(tr->fromAmount)));
        jtr.emplace_back(Pair("taker",          tr->toCurrency));
        jtr.emplace_back(Pair("taker_size",     xbridge::xBridgeStringValueFromAmount(tr->toAmount)));
        jtr.emplace_back(Pair("updated_at",     xbridge::iso8601(tr->txtime)));
        jtr.emplace_back(Pair("created_at",     xbridge::iso8601(tr->created)));
        jtr.emplace_back(Pair("status",         tr->strState()));
        result.emplace_back(jtr);

    }


    return uret(result);
}

UniValue dxGetOrderFills(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetOrderFills",
                "\n(maker) (taker) (combined, default=true)[optional]\n"
                "\nReturns all the recent trades by trade pair that have been filled \n"
                "(i.e. completed). Maker symbol is always listed first. The [combined] \n"
                "flag defaults to true. When set to false [combined] will return only \n"
                "maker trades, switch maker and taker to get the reverse.",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetOrderFills", "BLOCK LTC")
                  + HelpExampleRpc("dxGetOrderFills", "BLOCK LTC")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    bool invalidParams = ((params.size() != 2) &&
                          (params.size() != 3));
    if (invalidParams) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "(maker) (taker) (combined, default=true)[optional]"));

    }

    bool combined = params.size() == 3 ? params[2].get_bool() : true;

    const auto maker = params[0].get_str();
    const auto taker = params[1].get_str();



    TransactionMap history = xbridge::App::instance().history();



    TransactionVector result;

    for (auto &item : history) {
        const xbridge::TransactionDescrPtr &ptr = item.second;
        if ((ptr->state == xbridge::TransactionDescr::trFinished) &&
            (combined ? ((ptr->fromCurrency == maker && ptr->toCurrency == taker) || (ptr->toCurrency == maker && ptr->fromCurrency == taker)) : (ptr->fromCurrency == maker && ptr->toCurrency == taker))) {
            result.push_back(ptr);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const xbridge::TransactionDescrPtr &a,  const xbridge::TransactionDescrPtr &b)
    {
         return (a->txtime) > (b->txtime);
    });

    Array arr;
    for(const auto &transaction : result) {

        Object tmp;
        tmp.emplace_back(Pair("id",         transaction->id.GetHex()));
        tmp.emplace_back(Pair("time",       xbridge::iso8601(transaction->txtime)));
        tmp.emplace_back(Pair("maker",      transaction->fromCurrency));
        tmp.emplace_back(Pair("maker_size", xbridge::xBridgeStringValueFromAmount(transaction->fromAmount)));
        tmp.emplace_back(Pair("taker",      transaction->toCurrency));
        tmp.emplace_back(Pair("taker_size", xbridge::xBridgeStringValueFromAmount(transaction->toAmount)));
        arr.emplace_back(tmp);

    }
    return uret(arr);
}

UniValue dxGetOrderHistory(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetOrderHistory",
                "\n(maker) (taker) (start time) (end time) (granularity) (order_ids, default=false)[optional] (with_inverse, default=false)[optional] (limit, default="+std::to_string(xQuery::IntervalLimit{}.count())+")[optional]\n"
                "Returns the order history over a specified time interval."
                " [start_time] and [end_time] are \n"
                "in unix time seconds [granularity] in seconds of supported"
                " time interval lengths include: \n"
                + xQuery::supported_seconds_csv() + ". [order_ids] is a boolean,"
                " defaults to false (not showing ids).\n"
                "[with_inverse] is a boolean, defaults to false (not aggregating inverse currency pair).\n"
                "[limit] is the maximum number of intervals to return,"
                " default="+std::to_string(xQuery::IntervalLimit{}.count())+
                " maximum="+std::to_string(xQuery::IntervalLimit::max())+".\n"
                "[interval_timestamp] is one of [at_start | at_end], defaults to at_start (timestamp at start of the interval)[optional]\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetOrderHistory", "BLOCK LTC")
                  + HelpExampleRpc("dxGetOrderHistory", "BLOCK LTC")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    //--Validate query parameters
    if (params.size() < 5 || params.size() > 8)
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "(maker) (taker) (start time) (end time) (granularity) "
                               "(order_ids, default=false)[optional] "
                               "(with_inverse, default=false)[optional] "
                               "(limit, default="+std::to_string(xQuery::IntervalLimit{}.count())+")[optional]"
                               "(interval_timestamp, one of [at_start | at_end])[optional] "
                               ));
    const xQuery query{
        params[0].get_str(),    // maker
        params[1].get_str(),    // taker
        params[4].get_int(),    // granularity (need before start/end time)
        params[2].get_int64(),  // start time
        params[3].get_int64(),  // end time
        params.size() > 5 && params[5].get_bool()
            ? xQuery::WithTxids::Included
            : xQuery::WithTxids::Excluded,
        params.size() > 6 && params[6].get_bool()
            ? xQuery::WithInverse::Included
            : xQuery::WithInverse::Excluded,
        params.size() > 7
            ? xQuery::IntervalLimit{params[7].get_int()}
            : xQuery::IntervalLimit{},
        params.size() > 8
            ? xQuery::IntervalTimestamp{params[8].get_str()}
            : xQuery::IntervalTimestamp{}
    };

    if (query.error())
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, query.what() ));
    try {
        //--Process query, get result
        auto& xseries = xbridge::App::instance().getXSeriesCache();
        std::vector<xAggregate> result = xseries.getXAggregateSeries(query);

        //--Serialize result
        Array arr{};
        const boost::posix_time::time_duration offset = query.interval_timestamp.at_start()
            ? query.granularity
            : boost::posix_time::seconds{0};
        for (const auto& x : result) {
            double volume = x.fromVolume.amount<double>();
            Array ohlc{
                ArrayIL{xbridge::iso8601(x.timeEnd - offset), x.low, x.high, x.open, x.close, volume}
            };
            if (query.with_txids == xQuery::WithTxids::Included) {
                Array orderIds{};
                for (const auto& id : x.orderIds)
                    orderIds.emplace_back(id);
                ohlc.emplace_back(orderIds);
            }
            arr.emplace_back(ohlc);
        }
        return uret(arr);
    } catch(const std::exception& e) {
        return uret(xbridge::makeError(xbridge::UNKNOWN_ERROR, __FUNCTION__, e.what() ));
    } catch( ... ) {
        return uret(xbridge::makeError(xbridge::UNKNOWN_ERROR, __FUNCTION__, "unknown exception" ));
    }
}

UniValue dxGetOrder(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetOrder",
                "\nGet order info by id.\n",
                {
                    {"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Order id"},
                },
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetOrder", "aae6d7aedaed54ade57da4eda3e5d4a7de8a67d8e7a8d768ea567da5e467d4ea7a6d7a6d7a6d75a7d5a757da5")
                  + HelpExampleRpc("dxGetOrder", "aae6d7aedaed54ade57da4eda3e5d4a7de8a67d8e7a8d768ea567da5e467d4ea7a6d7a6d7a6d75a7d5a757da5")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() != 1) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, "(id)"));

    }

    uint256 id = uint256S(params[0].get_str());

    auto &xapp = xbridge::App::instance();

    const xbridge::TransactionDescrPtr order = xapp.transaction(uint256(id));

    if(order == nullptr) {

        return uret(xbridge::makeError(xbridge::TRANSACTION_NOT_FOUND, __FUNCTION__, id.ToString()));

    }

    xbridge::WalletConnectorPtr connFrom = xapp.connectorByCurrency(order->fromCurrency);
    xbridge::WalletConnectorPtr connTo   = xapp.connectorByCurrency(order->toCurrency);
    if(!connFrom) {

        return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, order->fromCurrency));

    }
    if (!connTo) {

        return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, order->toCurrency));

    }

    Object result;
    result.emplace_back(Pair("id",          order->id.GetHex()));
    result.emplace_back(Pair("maker",       order->fromCurrency));
    result.emplace_back(Pair("maker_size",  xbridge::xBridgeStringValueFromAmount(order->fromAmount)));
    result.emplace_back(Pair("taker",       order->toCurrency));
    result.emplace_back(Pair("taker_size",  xbridge::xBridgeStringValueFromAmount(order->toAmount)));
    result.emplace_back(Pair("updated_at",  xbridge::iso8601(order->txtime)));
    result.emplace_back(Pair("created_at",  xbridge::iso8601(order->created)));
    result.emplace_back(Pair("status",      order->strState()));
    return uret(result);
}

UniValue dxMakeOrder(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxMakeOrder",
                "\nCreate a new order. dryrun will validate the order without submitting the order to the network.\n",
                {
                    {"maker", RPCArg::Type::STR, RPCArg::Optional::NO, "Maker (e.g. LTC)"},
                    {"maker_size", RPCArg::Type::STR, RPCArg::Optional::NO, "Amount of maker coin being sent"},
                    {"maker_address", RPCArg::Type::STR, RPCArg::Optional::NO, "Maker address containing coin being sent"},
                    {"taker", RPCArg::Type::STR, RPCArg::Optional::NO, "Taker (e.g. BLOCK)"},
                    {"taker_size", RPCArg::Type::STR, RPCArg::Optional::NO, "Amount of taker coin being recieved"},
                    {"taker_address", RPCArg::Type::STR, RPCArg::Optional::NO, "Taker address receiving the coin"},
                    {"type", RPCArg::Type::STR, RPCArg::Optional::NO, "exact", "Order type (e.g. exact)"},
                    {"dryrun", RPCArg::Type::BOOL, "false", "Simulate the order submission without actually submitting the order, i.e. a test run"},
                },
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxMakeOrder", "LTC 25 LLZ1pgb6Jqx8hu84fcr5WC5HMoKRUsRE8H BLOCK 1000 BWQrvmuHB4C68KH5V7fcn9bFtWN8y5hBmR exact")
                  + HelpExampleRpc("dxMakeOrder", "LTC 25 LLZ1pgb6Jqx8hu84fcr5WC5HMoKRUsRE8H BLOCK 1000 BWQrvmuHB4C68KH5V7fcn9bFtWN8y5hBmR exact")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() < 7) {

        throw runtime_error("dxMakeOrder (maker) (maker size) (maker address) "
                            "(taker) (taker size) (taker address) (type) (dryrun)[optional]\n"
                            "Create a new order. dryrun will validate the order without submitting the order to the network.");

    }

    if (!xbridge::xBridgeValidCoin(params[1].get_str())) {
        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::INVALID_PARAMETERS,
                      "maker size is too precise, maximum precision supported is " +
                              std::to_string(xbridge::xBridgeSignificantDigits(xbridge::TransactionDescr::COIN)) + " digits")));
        error.emplace_back(Pair("code",     xbridge::INVALID_PARAMETERS));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    if (!xbridge::xBridgeValidCoin(params[4].get_str())) {
        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::INVALID_PARAMETERS,
                      "taker size is too precise, maximum precision supported is " +
                              std::to_string(xbridge::xBridgeSignificantDigits(xbridge::TransactionDescr::COIN)) + " digits")));
        error.emplace_back(Pair("code",     xbridge::INVALID_PARAMETERS));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    std::string fromCurrency    = params[0].get_str();
    double      fromAmount      = boost::lexical_cast<double>(params[1].get_str());
    std::string fromAddress     = params[2].get_str();

    std::string toCurrency      = params[3].get_str();
    double      toAmount        = boost::lexical_cast<double>(params[4].get_str());
    std::string toAddress       = params[5].get_str();

    std::string type            = params[6].get_str();

    // Validate the order type
    if (type != "exact") {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "Only the exact type is supported at this time."));

    }

    // Check that addresses are not the same
    if (fromAddress == toAddress) {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "maker address and taker address cannot be the same: " + fromAddress));
    }

    // Check upper limits
    if (fromAmount > (double)xbridge::TransactionDescr::MAX_COIN ||
            toAmount > (double)xbridge::TransactionDescr::MAX_COIN) {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "Maximum supported size is " + std::to_string(xbridge::TransactionDescr::MAX_COIN)));
    }
    // Check lower limits
    if (fromAmount <= 0 || toAmount <= 0) {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "Minimum supported size is " + xbridge::xBridgeStringValueFromPrice(1.0/xbridge::TransactionDescr::COIN)));
    }

    // Validate addresses
    xbridge::WalletConnectorPtr connFrom = xbridge::App::instance().connectorByCurrency(fromCurrency);
    xbridge::WalletConnectorPtr connTo   = xbridge::App::instance().connectorByCurrency(toCurrency);
    if (!connFrom) return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, "unable to connect to wallet: " + fromCurrency));
    if (!connTo) return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, "unable to connect to wallet: " + toCurrency));

    xbridge::App &app = xbridge::App::instance();

    if (!app.isValidAddress(fromAddress, connFrom)) {

        return uret(xbridge::makeError(xbridge::INVALID_ADDRESS, __FUNCTION__, fromAddress));

    }
    if (!app.isValidAddress(toAddress, connTo)) {

        return uret(xbridge::makeError(xbridge::INVALID_ADDRESS, __FUNCTION__, toAddress));

    }
    if(fromAmount <= .0) {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "maker size must be greater than 0"));
    }
    if(toAmount <= .0) {

        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "taker size must be greater than 0"));

    }
    // Perform explicit check on dryrun to avoid executing order on bad spelling
    bool dryrun = false;
    if (params.size() == 8) {
        std::string dryrunParam = params[7].get_str();
        if (dryrunParam != "dryrun") {

            return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, dryrunParam));

        }
        dryrun = true;
    }


    Object result;
    auto statusCode = app.checkCreateParams(fromCurrency, toCurrency,
                                       xbridge::xBridgeAmountFromReal(fromAmount), fromAddress);
    switch (statusCode) {
    case xbridge::SUCCESS:{
        // If dryrun
        if (dryrun) {
            result.emplace_back(Pair("id", uint256().GetHex()));
            result.emplace_back(Pair("maker", fromCurrency));
            result.emplace_back(Pair("maker_size",
                                     xbridge::xBridgeStringValueFromAmount(xbridge::xBridgeAmountFromReal(fromAmount))));
            result.emplace_back(Pair("maker_address", fromAddress));
            result.emplace_back(Pair("taker", toCurrency));
            result.emplace_back(Pair("taker_size",
                                     xbridge::xBridgeStringValueFromAmount(xbridge::xBridgeAmountFromReal(toAmount))));
            result.emplace_back(Pair("taker_address", toAddress));
            result.emplace_back(Pair("status", "created"));
            return uret(result);
        }
        break;
    }

    case xbridge::INVALID_CURRENCY: {
        return uret(xbridge::makeError(statusCode, __FUNCTION__, fromCurrency));
    }
    case xbridge::NO_SESSION:{
        return uret(xbridge::makeError(statusCode, __FUNCTION__, fromCurrency));
    }
    case xbridge::INSIFFICIENT_FUNDS:{
        return uret(xbridge::makeError(statusCode, __FUNCTION__, fromAddress));
    }

    default:
        return uret(xbridge::makeError(statusCode, __FUNCTION__));
    }


    uint256 id = uint256();
    uint256 blockHash = uint256();
    statusCode = xbridge::App::instance().sendXBridgeTransaction
          (fromAddress, fromCurrency, xbridge::xBridgeAmountFromReal(fromAmount),
           toAddress, toCurrency, xbridge::xBridgeAmountFromReal(toAmount), id, blockHash);

    if (statusCode == xbridge::SUCCESS) {

        Object obj;
        obj.emplace_back(Pair("id",             id.GetHex()));
        obj.emplace_back(Pair("maker_address",  fromAddress));
        obj.emplace_back(Pair("maker",          fromCurrency));
        obj.emplace_back(Pair("maker_size",     xbridge::xBridgeStringValueFromAmount(xbridge::xBridgeAmountFromReal(fromAmount))));
        obj.emplace_back(Pair("taker_address",  toAddress));
        obj.emplace_back(Pair("taker",          toCurrency));
        obj.emplace_back(Pair("taker_size",     xbridge::xBridgeStringValueFromAmount(xbridge::xBridgeAmountFromReal(toAmount))));
        const auto &createdTime = xbridge::App::instance().transaction(id)->created;
        obj.emplace_back(Pair("created_at",     xbridge::iso8601(createdTime)));
        obj.emplace_back(Pair("updated_at",     xbridge::iso8601(boost::posix_time::microsec_clock::universal_time()))); // TODO Need actual updated time, this is just estimate
        obj.emplace_back(Pair("block_id",       blockHash.GetHex()));
        obj.emplace_back(Pair("status",         "created"));
        return uret(obj);

    } else {
        return uret(xbridge::makeError(statusCode, __FUNCTION__));
    }
}

UniValue dxTakeOrder(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxTakeOrder",
                "\nAccepts the order. dryrun will evaluate input without accepting the order.\n",
                {
                    {"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Maker (e.g. LTC)"},
                    {"from_address", RPCArg::Type::STR, RPCArg::Optional::NO, "Address of coin being sent"},
                    {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "Address of coin being received"},
                    {"dryrun", RPCArg::Type::BOOL, "false", "Simulate the order acceptance without actually accepting the order, i.e. a test run"},
                },
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxTakeOrder", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872 LLZ1pgb6Jqx8hu84fcr5WC5HMoKRUsRE8H BWQrvmuHB4C68KH5V7fcn9bFtWN8y5hBmR")
                  + HelpExampleRpc("dxTakeOrder", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872 LLZ1pgb6Jqx8hu84fcr5WC5HMoKRUsRE8H BWQrvmuHB4C68KH5V7fcn9bFtWN8y5hBmR")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if ((params.size() != 3) && (params.size() != 4))
    {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "(id) (address from) (address to) [optional](dryrun)"));
    }

    uint256 id = uint256S(params[0].get_str());
    std::string fromAddress    = params[1].get_str();
    std::string toAddress      = params[2].get_str();

    xbridge::App &app = xbridge::App::instance();

    // Check that addresses are not the same
    if (fromAddress == toAddress) {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "address from and address to cannot be the same: " + fromAddress));
    }

    // Perform explicit check on dryrun to avoid executing order on bad spelling
    bool dryrun = false;
    if (params.size() == 4) {
        std::string dryrunParam = params[3].get_str();
        if (dryrunParam != "dryrun") {
            return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, dryrunParam));
        }
        dryrun = true;
    }

    Object result;
    xbridge::TransactionDescrPtr txDescr;
    auto statusCode = app.checkAcceptParams(id, txDescr, fromAddress);

    switch (statusCode)
    {
    case xbridge::SUCCESS: {
        if (txDescr->isLocal()) // no self trades
            return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__, "unable to accept your own order"));

        // taker [to] will match order [from] currency (due to pair swap happening later)
        xbridge::WalletConnectorPtr connTo = xbridge::App::instance().connectorByCurrency(txDescr->fromCurrency);
        // taker [from] will match order [to] currency (due to pair swap happening later)
        xbridge::WalletConnectorPtr connFrom   = xbridge::App::instance().connectorByCurrency(txDescr->toCurrency);
        if (!connFrom) return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, "unable to connect to wallet: " + txDescr->toCurrency));
        if (!connTo) return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, "unable to connect to wallet: " + txDescr->fromCurrency));
        // Check for valid toAddress
        if (!app.isValidAddress(toAddress, connTo))
            return uret(xbridge::makeError(xbridge::INVALID_ADDRESS, __FUNCTION__,
                                   ": " + txDescr->fromCurrency + " address is bad, are you using the correct address?"));
        // Check for valid fromAddress
        if (!app.isValidAddress(fromAddress, connFrom))
            return uret(xbridge::makeError(xbridge::INVALID_ADDRESS, __FUNCTION__,
                                   ": " + txDescr->toCurrency + " address is bad, are you using the correct address?"));

        if(dryrun)
        {
            result.emplace_back(Pair("id", uint256().GetHex()));

            result.emplace_back(Pair("maker", txDescr->fromCurrency));
            result.emplace_back(Pair("maker_size", xbridge::xBridgeStringValueFromAmount(txDescr->fromAmount)));

            result.emplace_back(Pair("taker", txDescr->toCurrency));
            result.emplace_back(Pair("taker_size", xbridge::xBridgeStringValueFromAmount(txDescr->toAmount)));

            result.emplace_back(Pair("updated_at", xbridge::iso8601(boost::posix_time::microsec_clock::universal_time())));
            result.emplace_back(Pair("created_at", xbridge::iso8601(txDescr->created)));

            result.emplace_back(Pair("status", "filled"));
            return uret(result);
        }

        break;
    }
    case xbridge::TRANSACTION_NOT_FOUND:
    {
        return uret(xbridge::makeError(xbridge::TRANSACTION_NOT_FOUND, __FUNCTION__, id.ToString()));
    }

    case xbridge::NO_SESSION:
    {
        return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, txDescr->toCurrency));
    }

    case xbridge::INSIFFICIENT_FUNDS:
    {
        return uret(xbridge::makeError(xbridge::INSIFFICIENT_FUNDS, __FUNCTION__, fromAddress));
    }

    default:
        return uret(xbridge::makeError(statusCode, __FUNCTION__));

    }

    // TODO swap is destructive on state (also complicates historical data)
    std::swap(txDescr->fromCurrency, txDescr->toCurrency);
    std::swap(txDescr->fromAmount, txDescr->toAmount);

    statusCode = app.acceptXBridgeTransaction(id, fromAddress, toAddress);
    if (statusCode == xbridge::SUCCESS) {

        result.emplace_back(Pair("id", id.GetHex()));

        result.emplace_back(Pair("maker", txDescr->fromCurrency));
        result.emplace_back(Pair("maker_size", xbridge::xBridgeStringValueFromAmount(txDescr->fromAmount)));

        result.emplace_back(Pair("taker", txDescr->toCurrency));
        result.emplace_back(Pair("taker_size", xbridge::xBridgeStringValueFromAmount(txDescr->toAmount)));

        result.emplace_back(Pair("updated_at", xbridge::iso8601(boost::posix_time::microsec_clock::universal_time())));
        result.emplace_back(Pair("created_at", xbridge::iso8601(txDescr->created)));

        result.emplace_back(Pair("status", txDescr->strState()));
        return uret(result);

    } else {
        // restore state on error
        std::swap(txDescr->fromCurrency, txDescr->toCurrency);
        std::swap(txDescr->fromAmount, txDescr->toAmount);
        return uret(xbridge::makeError(statusCode, __FUNCTION__));
    }
}

UniValue dxCancelOrder(const JSONRPCRequest& request)
{
    if(request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxCancelOrder",
                "\nCancel xbridge order.\n",
                {
                    {"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Order to cancel"},
                },
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxCancelOrder", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872")
                  + HelpExampleRpc("dxCancelOrder", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() != 1)
    {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               ""));
    }

    LOG() << "rpc cancel order " << __FUNCTION__;
    uint256 id = uint256S(params[0].get_str());

    xbridge::TransactionDescrPtr tx = xbridge::App::instance().transaction(id);
    if (!tx)
    {
        return uret(xbridge::makeError(xbridge::TRANSACTION_NOT_FOUND, __FUNCTION__, id.ToString()));
    }

    if (tx->state >= xbridge::TransactionDescr::trCreated)
    {
        return uret(xbridge::makeError(xbridge::INVALID_STATE, __FUNCTION__, "order is already " + tx->strState()));
    }

    const auto res = xbridge::App::instance().cancelXBridgeTransaction(id, crRpcRequest);
    if (res != xbridge::SUCCESS)
    {
        return uret(xbridge::makeError(res, __FUNCTION__));
    }

    xbridge::WalletConnectorPtr connFrom = xbridge::App::instance().connectorByCurrency(tx->fromCurrency);
    xbridge::WalletConnectorPtr connTo   = xbridge::App::instance().connectorByCurrency(tx->toCurrency);
    if (!connFrom) {
        return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, tx->fromCurrency));
    }

    if (!connTo) {
        return uret(xbridge::makeError(xbridge::NO_SESSION, __FUNCTION__, tx->toCurrency));
    }
    Object obj;
    obj.emplace_back(Pair("id", id.GetHex()));

    obj.emplace_back(Pair("maker", tx->fromCurrency));
    obj.emplace_back(Pair("maker_size", xbridge::xBridgeStringValueFromAmount(tx->fromAmount)));
    obj.emplace_back(Pair("maker_address", connFrom->fromXAddr(tx->from)));

    obj.emplace_back(Pair("taker", tx->toCurrency));
    obj.emplace_back(Pair("taker_size", xbridge::xBridgeStringValueFromAmount(tx->toAmount)));
    obj.emplace_back(Pair("taker_address", connTo->fromXAddr(tx->to)));
    obj.emplace_back(Pair("refund_tx", tx->refTx));

    obj.emplace_back(Pair("updated_at", xbridge::iso8601(tx->txtime)));
    obj.emplace_back(Pair("created_at", xbridge::iso8601(tx->created)));

    obj.emplace_back(Pair("status", tx->strState()));
    return uret(obj);
}

UniValue dxFlushCancelledOrders(const JSONRPCRequest& request)
{
    if(request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxFlushCancelledOrders",
                "\n(ageMillis)\n"
                "Flush cancelled orders older than ageMillis.\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxFlushCancelledOrders", "1568921382306")
                  + HelpExampleRpc("dxFlushCancelledOrders", "1568921382306")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    const int ageMillis = params.size() == 0
        ? 0
        : (params.size() == 1 ? params[0].get_int() : -1);

    if (ageMillis < 0)
    {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "(ageMillis)"));
    }

    const auto minAge = boost::posix_time::millisec{ageMillis};

    LOG() << "rpc flush cancelled orders older than " << minAge << ": " << __FUNCTION__;

    const auto now = boost::posix_time::microsec_clock::universal_time();
    const auto list = xbridge::App::instance().flushCancelledOrders(minAge);
    const auto micros = boost::posix_time::time_duration{ boost::posix_time::microsec_clock::universal_time() - now };

    Object result{
        Pair{"ageMillis",        ageMillis},
        Pair{"now",              xbridge::iso8601(now)},
        Pair{"durationMicrosec", static_cast<int>(micros.total_microseconds())},
    };
    Array a;
    for(const auto & it : list) {
        a.emplace_back(
            ArrayValue{Object{
                Pair{"id",        it.id.GetHex()},
                Pair{"txtime",    xbridge::iso8601(it.txtime)},
                Pair{"use_count", it.use_count},
            }}
        );
    }
    result.emplace_back("flushedOrders", a);
    return uret(result);
}

UniValue dxGetOrderBook(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetOrderBook",
                "\n(detail level, 1-4) (maker) (taker) (max orders, default=50)[optional]\n"
                "\nReturns the order book. There are 4 detail levels that can be specified to obtain \n"
                "different outputs for the orderbook. 1 lists the best bid and ask. 2 lists the \n"
                "aggregated bids and asks. 3 lists the non-aggregated bids and asks. 4 is level 1 \n"
                "with order ids. Optionally specify the maximum orders you wish to return.",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetOrderBook", "3 BLOCK LTC")
                  + HelpExampleRpc("dxGetOrderBook", "3 BLOCK LTC")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if ((params.size() < 3 || params.size() > 4))
    {
        return uret(xbridge::makeError(xbridge::INVALID_PARAMETERS, __FUNCTION__,
                               "(detail level, 1-4) (maker) (taker) (max orders, default=50)[optional]"));
    }

    Object res;
    TransactionMap trList = xbridge::App::instance().transactions();
    {
        /**
         * @brief detaiLevel - Get a list of open orders for a product.
         * The amount of detail shown can be customized with the level parameter.
         */
        const auto detailLevel  = params[0].get_int();
        const auto fromCurrency = params[1].get_str();
        const auto toCurrency   = params[2].get_str();

        std::size_t maxOrders = 50;

        if (params.size() == 4)
            maxOrders = params[3].get_int();

        if (maxOrders < 1)
            maxOrders = 1;

        if (detailLevel < 1 || detailLevel > 4)
        {
            return uret(xbridge::makeError(xbridge::INVALID_DETAIL_LEVEL, __FUNCTION__));
        }

        res.emplace_back(Pair("detail", detailLevel));
        res.emplace_back(Pair("maker", fromCurrency));
        res.emplace_back(Pair("taker", toCurrency));

        /**
         * @brief bids - array with bids
         */
        Array bids;
        /**
         * @brief asks - array with asks
         */
        Array asks;

        if(trList.empty())
        {
            LOG() << "empty transactions list";
            res.emplace_back(Pair("asks", asks));
            res.emplace_back(Pair("bids", bids));
            return uret(res);
        }

        TransactionMap asksList;
        TransactionMap bidsList;

        //copy all transactions in currencies specified in the parameters

        // ask orders are based in the first token in the trading pair
        std::copy_if(trList.begin(), trList.end(), std::inserter(asksList, asksList.end()),
                     [&toCurrency, &fromCurrency](const TransactionPair &transaction)
        {
            if(transaction.second == nullptr)
                return false;
            if (transaction.second->fromAmount <= 0 || transaction.second->toAmount <= 0)
                return false;
            if (transaction.second->state != xbridge::TransactionDescr::trPending)
                return false;

            return  ((transaction.second->toCurrency == toCurrency) &&
                    (transaction.second->fromCurrency == fromCurrency));
        });

        // bid orders are based in the second token in the trading pair (inverse of asks)
        std::copy_if(trList.begin(), trList.end(), std::inserter(bidsList, bidsList.end()),
                     [&toCurrency, &fromCurrency](const TransactionPair &transaction)
        {
            if(transaction.second == nullptr)
                return false;
            if (transaction.second->fromAmount <= 0 || transaction.second->toAmount <= 0)
                return false;
            if (transaction.second->state != xbridge::TransactionDescr::trPending)
                return false;

            return  ((transaction.second->toCurrency == fromCurrency) &&
                    (transaction.second->fromCurrency == toCurrency));
        });

        std::vector<xbridge::TransactionDescrPtr> asksVector;
        std::vector<xbridge::TransactionDescrPtr> bidsVector;

        for (const auto &trEntry : asksList)
            asksVector.emplace_back(trEntry.second);

        for (const auto &trEntry : bidsList)
            bidsVector.emplace_back(trEntry.second);

        // sort asks descending
        std::sort(asksVector.begin(), asksVector.end(),
                  [](const xbridge::TransactionDescrPtr &a, const xbridge::TransactionDescrPtr &b)
        {
            const auto priceA = xbridge::price(a);
            const auto priceB = xbridge::price(b);
            return priceA > priceB;
        });

        //sort bids descending
        std::sort(bidsVector.begin(), bidsVector.end(),
                  [](const xbridge::TransactionDescrPtr &a, const xbridge::TransactionDescrPtr &b)
        {
            const auto priceA = xbridge::priceBid(a);
            const auto priceB = xbridge::priceBid(b);
            return priceA > priceB;
        });

        // floating point comparisons
        // see Knuth 4.2.2 Eq 36
        auto floatCompare = [](const double a, const double b) -> bool
        {
            const auto epsilon = std::numeric_limits<double>::epsilon();
            return (fabs(a - b) / fabs(a) <= epsilon) && (fabs(a - b) / fabs(b) <= epsilon);
        };

        switch (detailLevel)
        {
        case 1:
        {
            //return only the best bid and ask
            if (!bidsList.empty()) {
                const auto bidsItem = std::max_element(bidsList.begin(), bidsList.end(),
                                       [](const TransactionPair &a, const TransactionPair &b)
                {
                    //find transaction with best bids
                    const auto &tr1 = a.second;
                    const auto &tr2 = b.second;

                    if(tr1 == nullptr)
                        return true;

                    if(tr2 == nullptr)
                        return false;

                    const auto priceA = xbridge::priceBid(tr1);
                    const auto priceB = xbridge::priceBid(tr2);

                    return priceA < priceB;
                });

                const auto bidsCount = std::count_if(bidsList.begin(), bidsList.end(),
                                                     [bidsItem, floatCompare](const TransactionPair &a)
                {
                    const auto &tr = a.second;

                    if(tr == nullptr)
                        return false;

                    const auto price = xbridge::priceBid(tr);

                    const auto &bestTr = bidsItem->second;
                    if (bestTr != nullptr)
                    {
                        const auto bestBidPrice = xbridge::priceBid(bestTr);
                        return floatCompare(price, bestBidPrice);
                    }

                    return false;
                });

                const auto &tr = bidsItem->second;
                if (tr != nullptr)
                {
                    const auto bidPrice = xbridge::priceBid(tr);
                    bids.emplace_back(Array{xbridge::xBridgeStringValueFromPrice(bidPrice),
                                            xbridge::xBridgeStringValueFromAmount(tr->toAmount),
                                            static_cast<int64_t>(bidsCount)});
                }
            }

            if (!asksList.empty()) {
                const auto asksItem = std::min_element(asksList.begin(), asksList.end(),
                                                   [](const TransactionPair &a, const TransactionPair &b)
                {
                    //find transactions with best asks
                    const auto &tr1 = a.second;
                    const auto &tr2 = b.second;

                    if(tr1 == nullptr)
                        return true;

                    if(tr2 == nullptr)
                        return false;

                    const auto priceA = xbridge::price(tr1);
                    const auto priceB = xbridge::price(tr2);
                    return priceA < priceB;
                });

                const auto asksCount = std::count_if(asksList.begin(), asksList.end(),
                                                     [asksItem, floatCompare](const TransactionPair &a)
                {
                    const auto &tr = a.second;

                    if(tr == nullptr)
                        return false;

                    const auto price = xbridge::price(tr);

                    const auto &bestTr = asksItem->second;
                    if (bestTr != nullptr)
                    {
                        const auto bestAskPrice = xbridge::price(bestTr);
                        return floatCompare(price, bestAskPrice);
                    }

                    return false;
                });

                const auto &tr = asksItem->second;
                if (tr != nullptr)
                {
                    const auto askPrice = xbridge::price(tr);
                    asks.emplace_back(Array{xbridge::xBridgeStringValueFromPrice(askPrice),
                                            xbridge::xBridgeStringValueFromAmount(tr->fromAmount),
                                            static_cast<int64_t>(asksCount)});
                }
            }

            res.emplace_back(Pair("asks", asks));
            res.emplace_back(Pair("bids", bids));
            return uret(res);
        }
        case 2:
        {
            //Top X bids and asks (aggregated)

            /**
             * @brief bound - calculate upper bound
             */
            auto bound = std::min<int32_t>(maxOrders, bidsVector.size());
            for (size_t i = 0; i < bound; ++i) // Best bids are at the beginning of the stack (sorted descending, highest price better)
            {
                if(bidsVector[i] == nullptr)
                    continue;

                Array bid;
                //calculate bids and push to array
                const auto bidAmount    = bidsVector[i]->toAmount;
                const auto bidPrice     = xbridge::priceBid(bidsVector[i]);
                auto bidSize            = bidAmount;
                const auto bidsCount    = std::count_if(bidsList.begin(), bidsList.end(),
                                                     [bidPrice, floatCompare](const TransactionPair &a)
                {
                    const auto &tr = a.second;

                    if(tr == nullptr)
                        return false;

                    const auto price = xbridge::priceBid(tr);

                    return floatCompare(price, bidPrice);
                });
                //array sorted by bid price, we can to skip the transactions with equals bid price
                while((++i < bound) && floatCompare(xbridge::priceBid(bidsVector[i]), bidPrice)) {
                    bidSize += bidsVector[i]->toAmount;
                }
                bid.emplace_back(xbridge::xBridgeStringValueFromPrice(bidPrice));
                bid.emplace_back(xbridge::xBridgeStringValueFromAmount(bidSize));
                bid.emplace_back(static_cast<int64_t>(bidsCount));
                bids.emplace_back(bid);
            }

            bound = std::min<int32_t>(maxOrders, asksVector.size());
            const auto asks_len = static_cast<int32_t>(asksVector.size());
            for (int32_t i = asks_len - bound; i < asks_len; ++i) // Best asks are at the back of the stack (sorted descending, lowest price better)
            {
                if(asksVector[i] == nullptr)
                    continue;

                Array ask;
                //calculate asks and push to array
                const auto askAmount    = asksVector[i]->fromAmount;
                const auto askPrice     = xbridge::price(asksVector[i]);
                auto askSize            = askAmount;
                const auto asksCount    = std::count_if(asksList.begin(), asksList.end(),
                                                     [askPrice, floatCompare](const TransactionPair &a)
                {
                    const auto &tr = a.second;

                    if(tr == nullptr)
                        return false;

                    const auto price = xbridge::price(tr);

                    return floatCompare(price, askPrice);
                });

                //array sorted by price, we can to skip the transactions with equals price
                while((++i < bound) && floatCompare(xbridge::price(asksVector[i]), askPrice)){
                    askSize += asksVector[i]->fromAmount;
                }
                ask.emplace_back(xbridge::xBridgeStringValueFromPrice(askPrice));
                ask.emplace_back(xbridge::xBridgeStringValueFromAmount(askSize));
                ask.emplace_back(static_cast<int64_t>(asksCount));
                asks.emplace_back(ask);
            }

            res.emplace_back(Pair("asks", asks));
            res.emplace_back(Pair("bids", bids));
            return uret(res);
        }
        case 3:
        {
            //Full order book (non aggregated)
            auto bound = std::min<int32_t>(maxOrders, bidsVector.size());
            for (size_t i = 0; i < bound; ++i) // Best bids are at the beginning of the stack (sorted descending, highest price better)
            {
                if(bidsVector[i] == nullptr)
                    continue;

                Array bid;
                const auto bidAmount   = bidsVector[i]->toAmount;
                const auto bidPrice    = xbridge::priceBid(bidsVector[i]);
                bid.emplace_back(xbridge::xBridgeStringValueFromPrice(bidPrice));
                bid.emplace_back(xbridge::xBridgeStringValueFromAmount(bidAmount));
                bid.emplace_back(bidsVector[i]->id.GetHex());

                bids.emplace_back(bid);
            }

            bound = std::min<int32_t>(maxOrders, asksVector.size());
            const auto asks_len = static_cast<int32_t>(asksVector.size());
            for (int32_t i = asks_len - bound; i < asks_len; ++i) // Best asks are at the back of the stack (sorted descending, lowest price better)
            {
                if(asksVector[i] == nullptr)
                    continue;

                Array ask;
                const auto bidAmount    = asksVector[i]->fromAmount;
                const auto askPrice     = xbridge::price(asksVector[i]);
                ask.emplace_back(xbridge::xBridgeStringValueFromPrice(askPrice));
                ask.emplace_back(xbridge::xBridgeStringValueFromAmount(bidAmount));
                ask.emplace_back(asksVector[i]->id.GetHex());

                asks.emplace_back(ask);
            }

            res.emplace_back(Pair("asks", asks));
            res.emplace_back(Pair("bids", bids));
            return uret(res);
        }
        case 4:
        {
            //return Only the best bid and ask
            if (!bidsList.empty()) {
                const auto bidsItem = std::max_element(bidsList.begin(), bidsList.end(),
                                           [](const TransactionPair &a, const TransactionPair &b)
                {
                    //find transaction with best bids
                    const auto &tr1 = a.second;
                    const auto &tr2 = b.second;

                    if(tr1 == nullptr)
                        return true;

                    if(tr2 == nullptr)
                        return false;

                    const auto priceA = xbridge::priceBid(tr1);
                    const auto priceB = xbridge::priceBid(tr2);

                    return priceA < priceB;
                });

                const auto &tr = bidsItem->second;
                if (tr != nullptr)
                {
                    const auto bidPrice = xbridge::priceBid(tr);
                    bids.emplace_back(xbridge::xBridgeStringValueFromPrice(bidPrice));
                    bids.emplace_back(xbridge::xBridgeStringValueFromAmount(tr->toAmount));

                    Array bidsIds;
                    bidsIds.emplace_back(tr->id.GetHex());

                    for(const TransactionPair &tp : bidsList)
                    {
                        const auto &otherTr = tp.second;

                        if(otherTr == nullptr)
                            continue;

                        if(tr->id == otherTr->id)
                            continue;

                        const auto otherTrBidPrice = xbridge::priceBid(otherTr);

                        if(!floatCompare(bidPrice, otherTrBidPrice))
                            continue;

                        bidsIds.emplace_back(otherTr->id.GetHex());
                    }

                    bids.emplace_back(bidsIds);
                }
            }

            if (!asksList.empty()) {
                const auto asksItem = std::min_element(asksList.begin(), asksList.end(),
                                                   [](const TransactionPair &a, const TransactionPair &b)
                {
                    //find transactions with best asks
                    const auto &tr1 = a.second;
                    const auto &tr2 = b.second;

                    if(tr1 == nullptr)
                        return true;

                    if(tr2 == nullptr)
                        return false;

                    const auto priceA = xbridge::price(tr1);
                    const auto priceB = xbridge::price(tr2);
                    return priceA < priceB;
                });

                const auto &tr = asksItem->second;
                if (tr != nullptr)
                {
                    const auto askPrice = xbridge::price(tr);
                    asks.emplace_back(xbridge::xBridgeStringValueFromPrice(askPrice));
                    asks.emplace_back(xbridge::xBridgeStringValueFromAmount(tr->fromAmount));

                    Array asksIds;
                    asksIds.emplace_back(tr->id.GetHex());

                    for(const TransactionPair &tp : asksList)
                    {
                        const auto &otherTr = tp.second;

                        if(otherTr == nullptr)
                            continue;

                        if(tr->id == otherTr->id)
                            continue;

                        const auto otherTrAskPrice = xbridge::price(otherTr);

                        if(!floatCompare(askPrice, otherTrAskPrice))
                            continue;

                        asksIds.emplace_back(otherTr->id.GetHex());
                    }

                    asks.emplace_back(asksIds);
                }
            }

            res.emplace_back(Pair("asks", asks));
            res.emplace_back(Pair("bids", bids));
            return uret(res);
        }

        default:
            return uret(xbridge::makeError(xbridge::INVALID_DETAIL_LEVEL, __FUNCTION__));
        }
    }
}

UniValue dxGetMyOrders(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetMyOrders",
                "\nLists all orders owned by you.\n",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetMyOrders", "")
                  + HelpExampleRpc("dxGetMyOrders", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (!params.empty()) {

        Object error;
        error.emplace_back(Pair("error",
                                            xbridge::xbridgeErrorText(xbridge::INVALID_PARAMETERS,
                                                                      "This function does not accept any parameters")));
        error.emplace_back(Pair("code",     xbridge::INVALID_PARAMETERS));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);

    }

    xbridge::App & xapp = xbridge::App::instance();

    Array r;
    TransactionVector orders;

    TransactionMap trList = xbridge::App::instance().transactions();

    // Filter local orders
    for (auto i : trList) {
        const xbridge::TransactionDescrPtr &t = i.second;
        if(!t->isLocal())
            continue;
        orders.push_back(t);
    }

    // Add historical orders
    TransactionMap history = xbridge::App::instance().history();

    // Filter local orders only
    for (auto &item : history) {
        const xbridge::TransactionDescrPtr &ptr = item.second;
        if (ptr->isLocal() &&
                (ptr->state == xbridge::TransactionDescr::trFinished ||
                 ptr->state == xbridge::TransactionDescr::trCancelled)) {
            orders.push_back(ptr);
        }
    }

    // Return if no records
    if (orders.empty())
        return uret(r);

    // sort ascending by updated time
    std::sort(orders.begin(), orders.end(),
        [](const xbridge::TransactionDescrPtr &a,  const xbridge::TransactionDescrPtr &b) {
            return (a->txtime) < (b->txtime);
        });

    std::map<std::string, bool> seen;
    for (const auto &t : orders) {
        // do not process already seen orders
        if (seen.count(t->id.GetHex()))
            continue;
        seen[t->id.GetHex()] = true;

        xbridge::WalletConnectorPtr connFrom = xapp.connectorByCurrency(t->fromCurrency);
        xbridge::WalletConnectorPtr connTo   = xapp.connectorByCurrency(t->toCurrency);

        std::string makerAddress;
        std::string takerAddress;
        if (connFrom)
            makerAddress = connFrom->fromXAddr(t->from);
        if (connTo)
            takerAddress = connTo->fromXAddr(t->to);

        Object o;
        o.emplace_back(Pair("id", t->id.GetHex()));

        // maker data
        o.emplace_back(Pair("maker", t->fromCurrency));
        o.emplace_back(Pair("maker_size", xbridge::xBridgeStringValueFromAmount(t->fromAmount)));
        o.emplace_back(Pair("maker_address", makerAddress));
        // taker data
        o.emplace_back(Pair("taker", t->toCurrency));
        o.emplace_back(Pair("taker_size", xbridge::xBridgeStringValueFromAmount(t->toAmount)));
        o.emplace_back(Pair("taker_address", takerAddress));
        // dates
        o.emplace_back(Pair("updated_at", xbridge::iso8601(t->txtime)));
        o.emplace_back(Pair("created_at", xbridge::iso8601(t->created)));
        o.emplace_back(Pair("status", t->strState()));

        r.emplace_back(o);
    }

    return uret(r);
}

UniValue dxGetTokenBalances(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetTokenBalances",
                "\nList of connected wallet balances. These balances do not include orders that are using \n"
                "locked utxos to support a pending or open order. The DX works best with presliced utxos \n"
                "so that your entire wallet balance is capable of multiple simultaneous trades.",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetTokenBalances", "")
                  + HelpExampleRpc("dxGetTokenBalances", "")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() != 0)
    {
        Object error;
        error.emplace_back(Pair("error",
                                            xbridge::xbridgeErrorText(xbridge::INVALID_PARAMETERS,
                                                                      "This function does not accept any parameters")));
        error.emplace_back(Pair("code",     xbridge::INVALID_PARAMETERS));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    Object res;

    // Wallet balance
    double walletBalance = boost::numeric_cast<double>(xbridge::availableBalance()) / boost::numeric_cast<double>(COIN);
    res.emplace_back("Wallet", xbridge::xBridgeStringValueFromPrice(walletBalance));

    // Add connected wallet balances
    const auto &connectors = xbridge::App::instance().connectors();
    for(const auto &connector : connectors)
    {
        const auto & excluded = xbridge::App::instance().getAllLockedUtxos(connector->currency);
        const auto balance = connector->getWalletBalance(excluded);

        //ignore not connected wallets
        if(balance >= 0)
            res.emplace_back(connector->currency, xbridge::xBridgeStringValueFromPrice(balance));
    }

    return uret(res);
}

UniValue dxGetLockedUtxos(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            RPCHelpMan{"dxGetLockedUtxos",
                "\n(id)\n"
                "Return list of locked utxo of an order.",
                {},
                RPCResult{
                "\n"
                },
                RPCExamples{
                    HelpExampleCli("dxGetLockedUtxos", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872")
                  + HelpExampleRpc("dxGetLockedUtxos", "e1e493130d784d6ce22e4976962d9837c7a671555b0cf78b022dfdf861496872")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    if (params.size() > 1)
    {
        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::INVALID_PARAMETERS, "requered transaction id or empty param")));
        error.emplace_back(Pair("code",     xbridge::INVALID_PARAMETERS));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    xbridge::Exchange & e = xbridge::Exchange::instance();
    if (!e.isStarted())
    {
        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::Error::NOT_EXCHANGE_NODE)));
        error.emplace_back(Pair("code",     xbridge::Error::NOT_EXCHANGE_NODE));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    uint256 id;

    if(params.size() == 1)
        id = uint256S(params[0].get_str());

    std::vector<xbridge::wallet::UtxoEntry> items;
    if(!e.getUtxoItems(id, items))
    {

        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::Error::TRANSACTION_NOT_FOUND, id.GetHex())));
        error.emplace_back(Pair("code",     xbridge::Error::TRANSACTION_NOT_FOUND));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    Array utxo;

    for(const xbridge::wallet::UtxoEntry & entry : items)
        utxo.emplace_back(entry.toString());

    Object obj;
    if(id.IsNull())
    {
        obj.emplace_back(Pair("all_locked_utxo", utxo));

        return uret(obj);
    }

    xbridge::TransactionPtr pendingTx = e.pendingTransaction(id);
    xbridge::TransactionPtr acceptedTx = e.transaction(id);

    if (!pendingTx->isValid() && !acceptedTx->isValid())
    {
        Object error;
        error.emplace_back(Pair("error",    xbridge::xbridgeErrorText(xbridge::Error::TRANSACTION_NOT_FOUND, id.GetHex())));
        error.emplace_back(Pair("code",     xbridge::Error::TRANSACTION_NOT_FOUND));
        error.emplace_back(Pair("name",     __FUNCTION__));
        return uret(error);
    }

    obj.emplace_back(Pair("id", id.GetHex()));

    if(pendingTx->isValid())
        obj.emplace_back(Pair(pendingTx->a_currency(), utxo));
    else if(acceptedTx->isValid())
        obj.emplace_back(Pair(acceptedTx->a_currency() + "_and_" + acceptedTx->b_currency(), utxo));

    return uret(obj);
}

UniValue gettradingdata(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            RPCHelpMan{"gettradingdata",
                "\nReturns an object containing xbridge trading records.\n",
                {
                    {"blocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "count of blocks for search"},
                    {"errors", RPCArg::Type::BOOL, "false", "show errors"},
                },
                RPCResult{
                "{\n"
                "  \"timestamp\":  \"timestamp\",       (uint64) block date in unixtime format\n"
                "  \"txid\":       \"transaction id\",  (string) blocknet transaction id\n"
                "  \"to\":         \"address\",         (string) receiver address\n"
                "  \"xid\":        \"transaction id\",  (string) xbridge transaction id\n"
                "  \"from\":       \"XXX\",             (string) from currency\n"
                "  \"fromAmount\": 0,                   (uint64) from amount\n"
                "  \"to\":         \"XXX\",             (string) to currency\n"
                "  \"toAmount\":   0,                   (uint64) toAmount\n"
                "}\n"
                },
                RPCExamples{
                    HelpExampleCli("gettradingdata", "")
                  + HelpExampleRpc("gettradingdata", "")
                  + HelpExampleCli("gettradingdata", "43200")
                  + HelpExampleRpc("gettradingdata", "43200")
                },
            }.ToString());
    Value js; json_spirit::read_string(request.params.write(), js); Array params = js.get_array();

    uint32_t countOfBlocks = std::numeric_limits<uint32_t>::max();
    if (params.size() >= 1)
    {
        RPCTypeCheck(request.params, {UniValue::VNUM});
        countOfBlocks = params[0].get_int();
    }
    bool showErrors = false;
    if (params.size() == 2) {
        RPCTypeCheck(request.params, {UniValue::VBOOL});
        showErrors = params[1].get_bool();
    }

    LOCK(cs_main);

    Array records;

    CBlockIndex * pindex = chainActive.Tip();
    int64_t timeBegin = chainActive.Tip()->GetBlockTime();
    for (; pindex->pprev && pindex->GetBlockTime() > (timeBegin-30*24*60*60) && countOfBlocks > 0;
             pindex = pindex->pprev, --countOfBlocks)
    {
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus()))
        {
            // throw
            continue;
        }
        const auto timestamp = block.GetBlockTime();
        for (const CTransactionRef & tx : block.vtx)
        {
            const auto txid = tx->GetHash().GetHex();
            std::string snode_pubkey{};

            const CurrencyPair p = TxOutToCurrencyPair(tx->vout, snode_pubkey);
            switch(p.tag) {
            case CurrencyPair::Tag::Error:
                // Show errors
                if (showErrors)
                    records.emplace_back(Object{
                        Pair{"timestamp",  timestamp},
                        Pair{"txid",       txid},
                        Pair{"xid",        p.error()}
                    });
                break;
            case CurrencyPair::Tag::Valid:
                records.emplace_back(Object{
                            Pair{"timestamp",  timestamp},
                            Pair{"txid",       txid},
                            Pair{"to",         snode_pubkey},
                            Pair{"xid",        p.xid()},
                            Pair{"from",       p.from.currency().to_string()},
                            Pair{"fromAmount", p.from.amount<double>()},
                            Pair{"to",         p.to.currency().to_string()},
                            Pair{"toAmount",   p.to.amount<double>()},
                            });
                break;
            case CurrencyPair::Tag::Empty:
            default:
                break;
            }
        }
    }

    return uret(records);
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                       actor (function)          argNames
  //  --------------------- -------------------------- ------------------------- ----------
    { "xbridge",            "dxGetOrderFills",         &dxGetOrderFills,         {} },
    { "xbridge",            "dxGetOrders",             &dxGetOrders,             {} },
    { "xbridge",            "dxGetOrder",              &dxGetOrder,              {} },
    { "xbridge",            "dxGetLocalTokens",        &dxGetLocalTokens,        {} },
    { "xbridge",            "dxLoadXBridgeConf",       &dxLoadXBridgeConf,       {} },
    { "xbridge",            "dxGetNewTokenAddress",    &dxGetNewTokenAddress,    {} },
    { "xbridge",            "dxGetNetworkTokens",      &dxGetNetworkTokens,      {} },
    { "xbridge",            "dxMakeOrder",             &dxMakeOrder,             {} },
    { "xbridge",            "dxTakeOrder",             &dxTakeOrder,             {} },
    { "xbridge",            "dxCancelOrder",           &dxCancelOrder,           {} },
    { "xbridge",            "dxGetOrderHistory",       &dxGetOrderHistory,       {} },
    { "xbridge",            "dxGetOrderBook",          &dxGetOrderBook,          {} },
    { "xbridge",            "dxGetTokenBalances",      &dxGetTokenBalances,      {} },
    { "xbridge",            "dxGetMyOrders",           &dxGetMyOrders,           {} },
    { "xbridge",            "dxGetLockedUtxos",        &dxGetLockedUtxos,        {} },
    { "xbridge",            "dxFlushCancelledOrders",  &dxFlushCancelledOrders,  {} },
    { "xbridge",            "gettradingdata",          &gettradingdata,          {} },
};
// clang-format on

void RegisterXBridgeRPCCommands(CRPCTable &t)
{
    for (const auto & command : commands)
        t.appendCommand(command.name, &command);
}