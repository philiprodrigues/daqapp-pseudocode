/**
 * @file FakeHLF_PseudoCode.cpp FakeHLF_PseudoCode class
 * implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "FakeHLF_PseudoCode.hpp"

#include <ers/ers.h>
#include "TRACE/trace.h"

#include <chrono>
#include <thread>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
#define TRACE_NAME "FakeHLF_PseudoCode" // NOLINT
#define TLVL_ENTER_EXIT_METHODS 10
#define TLVL_WORK_PROGRESS 15

namespace dunedaq {
namespace fakehlf {

FakeHLF_PseudoCode::FakeHLF_PseudoCode(const std::string& name)
  : DAQModule(name)
  , thread_(std::bind(&FakeHLF_PseudoCode::do_work, this, std::placeholders::_1))
  , requestChannel_(nullptr)
  , dataSource_(nullptr)
  , resultSink_(nullptr)
  , requestSendTimeout_(100)
  , dataReceiveTimeout_(100)
  , resultSendTimeout_(100)
{
  register_command("start", &FakeHLF_PseudoCode::do_start);
  register_command("stop", &FakeHLF_PseudoCode::do_stop);
}

void
FakeHLF_PseudoCode::init()
{

  // 02-Jun-2020, KAB: the way that I've written this code implies that the
  // QueueRegistry evolves into some sort of more general inter-connection registry *and*
  // we can ask it to return named network connections as well as named queues.
  // Question 1: is this what we want?
  // Question 2: can all types of network connections be created 'whenever'?

  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  try
  {
    requestChannel_.reset(new dunedaq::appfwk::DAQSink<std::vector<uint8_t>>(get_config()["request_channel_name"].get<std::string>()));
  }
  catch (const ers::Issue& excpt)
  {
    throw InvalidEndpointFatalError(ERS_HERE, get_name(), "request channel", excpt);
  }

  try
  {
    dataSource_.reset(new dunedaq::appfwk::DAQSource<std::vector<uint8_t>>(get_config()["data_source_name"].get<std::string>()));
  }
  catch (const ers::Issue& excpt)
  {
    throw InvalidEndpointFatalError(ERS_HERE, get_name(), "data source", excpt);
  }

  try
  {
    resultSink_.reset(new dunedaq::appfwk::DAQSink<std::vector<uint8_t>>(get_config()["result_sink_name"].get<std::string>()));
  }
  catch (const ers::Issue& excpt)
  {
    throw InvalidEndpointFatalError(ERS_HERE, get_name(), "result sink", excpt);
  }

  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
FakeHLF_PseudoCode::do_start(const std::vector<std::string>& /*args*/)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  thread_.start_working_thread();
  ERS_LOG(get_name() << " successfully started");
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
FakeHLF_PseudoCode::do_stop(const std::vector<std::string>& /*args*/)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  thread_.stop_working_thread();
  ERS_LOG(get_name() << " successfully stopped");
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
FakeHLF_PseudoCode::do_work(std::atomic<bool>& running_flag)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_work() method";
  const int TRsToRequestEachTime = 1;
  int requestCount = 0;
  int requestedTRCount = 0;
  int receivedcount = 0;
  int sentCount = 0;

  while (running_flag.load()) {
    TLOG(TLVL_WORK_PROGRESS) << get_name() << ": Sending a request for another Trigger Record";

    // *** Create the request message
    TriggerRecordRequest trReq;
    // how should we specify "my address"?
    trReq.setMyAddress(get_config()["data_source_name"].get<std::string>());
    // we don't need to go into details on specifying the acceptable TR types now
    trReq.setAcceptableTRTypes(bitmaskOrStructureOrList);
    trReq.setNumberOfRecordsToSend(TRsToRequestEachTime);

    // ** Prepare the message to be sent to the Dispatcher
    std::vector<uint8_t> trrBuff;
#if A
    // One model for serializing the message (I'm trying to illustrate using generated code)
    TriggerRecordRequestSerializer::serialize(trReq, trrBuff);
#elif B
    // Would it be possible to have some sort of general-purposer serializer that
    // can handle different type of messages?
    serializationWorker_.serialize(trReq, trrBuff);
#endif

    // *** Send the message to the Dispatcher
    try
    {
      requestChannel_->push(trrBuff, queueTimeout_);
    }
    catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt)
    {
      std::ostringstream oss_warn;
      oss_warn << "push to request channel \"" << requestChannel_->get_name() << "\"";
      ers::warning(dunedaq::appfwk::QueueTimeoutExpired(ERS_HERE, get_name(), oss_warn.str(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(queueTimeout_).count()));
      continue;
    }

    // provide an update on progress
    ++requestCount;
    requestTRCount += TRsToRequestEachTime;
    TLOG(TLVL_WORK_PROGRESS) << get_name() << ": " << requestCount << " requests sent, waiting for "
                             << TRsToRequestEachTime << " Trigger Records to be received in response to this latest request";

    //
    int trigRecLeftToReceive = TRsToRequestEachTime;
    while (trigRecLeftToReceive > 0 && running_flag.load())
    {
      // allocate a buffer to receive the Trigger Record into
      std::vector<uint8_t> receiveBuff;
      receiveBuff.resize(MAX_TR_SIZE);

      bool trigRecWasSuccessfullyReceived = false;
      while (!trigRecWasSuccessfullyReceived && running_flag.load())
      {
        TLOG(TLVL_LIST_VALIDATION) << get_name() << ": Receiving the next Trigger Record";
        try
        {
          dataSource_->pop(receiveBuff, queueTimeout_);
          trigRecWasSuccessfullyReceived = true;
          --trigRecLeftToReceive;
        }
        catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt)
        {
          // only print out a gentle warning after a while.
          // It's OK that data doesn't arrive promptly (maybe the current trigger rate is very low),
          // but it's probably good to let someone know that we're waiting for data after a while.
          std::ostringstream oss_warn;
          oss_warn << "pop from data source";
          ers::warning(dunedaq::appfwk::QueueTimeoutExpired(ERS_HERE, get_name(), oss_warn.str(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(queueTimeout_).count()));
        }
      }

      if (trigRecWasSuccessfullyReceived)
      {
        // process the data in some way.
        // For now, I'll just copy the data from the input to the output.
        std::vector<uint8_t> sendBuff;
        sendBuff.resize(MAX_TR_SIZE);
        std::copy(receiveBuff.begin(), receiveBuff.end(), sendBuff.start());

        bool trigRecWasSuccessfullySent = false;
        while (!trigRecWasSuccessfullySent && running_flag.load())
        {
          TLOG(TLVL_LIST_VALIDATION) << get_name() << ": Sending the processed Trigger Record back to the Dispatcher";
          try
          {
            resultSink_->push(sendBuff, queueTimeout_);
            trigRecWasSuccessfullySent = true;
          }
          catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt)
          {
            // Complain loudly if we can't send the result to the Dispatcher.
            // We'll need to decide whether to retry forever, or go onto the next input TR.
          }
        }
      }
    }

    TLOG(TLVL_WORK_PROGRESS) << get_name() << ": End of do_work loop";
  }

  std::ostringstream oss_summ;
  oss_summ << ": Exiting do_work() method, sent " << requestCount << " requests for data, each of them "
	   << "requesting " << TRsPerRequest << " trigger records. Received " << receivedCount
           << " Trigger Records, successfully processed " << processedCount << " of them, and "
           << "successfully sent the results for " << sentCount << of them back to the Dispatcher.";
           << " lists and successfully sent " << sentCount << ". ";dd
  ers::info(ProgressUpdate(ERS_HERE, get_name(), oss_summ.str()));
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_work() method";
}

} // namespace fakehlf
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::fakehlf::FakeHLF_PseudoCode)
