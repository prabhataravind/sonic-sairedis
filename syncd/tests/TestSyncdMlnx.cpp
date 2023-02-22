// includes -----------------------------------------------------------------------------------------------------------

#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <thread>

#include <gtest/gtest.h>

#include <swss/logger.h>

#include "Sai.h"
#include "Syncd.h"
#include "MetadataLogger.h"

#include "TestSyncdLib.h"

using namespace syncd;

// functions ----------------------------------------------------------------------------------------------------------

static const char* profile_get_value(
    _In_ sai_switch_profile_id_t profile_id,
    _In_ const char* variable)
{
    SWSS_LOG_ENTER();

    return NULL;
}

static int profile_get_next_value(
    _In_ sai_switch_profile_id_t profile_id,
    _Out_ const char** variable,
    _Out_ const char** value)
{
    SWSS_LOG_ENTER();

    if (value == NULL)
    {
        SWSS_LOG_INFO("resetting profile map iterator");
        return 0;
    }

    if (variable == NULL)
    {
        SWSS_LOG_WARN("variable is null");
        return -1;
    }

    SWSS_LOG_INFO("iterator reached end");
    return -1;
}

static sai_service_method_table_t test_services = {
    profile_get_value,
    profile_get_next_value
};

// Nvidia ASIC --------------------------------------------------------------------------------------------------------

void syncdMlnxWorkerThread()
{
    SWSS_LOG_ENTER();

    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_NOTICE);
    MetadataLogger::initialize();

    auto vendorSai = std::make_shared<VendorSai>();
    auto commandLineOptions = std::make_shared<CommandLineOptions>();
    auto isWarmStart = false;

    commandLineOptions->m_enableSyncMode= true;
    commandLineOptions->m_enableTempView = false;
    commandLineOptions->m_disableExitSleep = true;
    commandLineOptions->m_enableUnittests = false;
    commandLineOptions->m_enableSaiBulkSupport = true;
    commandLineOptions->m_startType = SAI_START_TYPE_COLD_BOOT;
    commandLineOptions->m_redisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC;
    commandLineOptions->m_profileMapFile = "./mlnx/sai.profile";

    auto syncd = std::make_shared<Syncd>(vendorSai, commandLineOptions, isWarmStart);
    syncd->run();

    SWSS_LOG_NOTICE("Started syncd worker");
}

class SyncdMlnxTest : public ::testing::Test
{
public:
    SyncdMlnxTest() = default;
    virtual ~SyncdMlnxTest() = default;

public:
    virtual void SetUp() override
    {
        SWSS_LOG_ENTER();

        // flush ASIC DB

        flushAsicDb();

        // start syncd worker

        m_worker = std::make_shared<std::thread>(syncdMlnxWorkerThread);

        // initialize SAI redis

        m_sairedis = std::make_shared<sairedis::Sai>();

        auto status = m_sairedis->initialize(0, &test_services);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // set communication mode

        sai_attribute_t attr;

        attr.id = SAI_REDIS_SWITCH_ATTR_REDIS_COMMUNICATION_MODE;
        attr.value.s32 = SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC;

        status = m_sairedis->set(SAI_OBJECT_TYPE_SWITCH, SAI_NULL_OBJECT_ID, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // enable recording

        attr.id = SAI_REDIS_SWITCH_ATTR_RECORD;
        attr.value.booldata = true;

        status = m_sairedis->set(SAI_OBJECT_TYPE_SWITCH, SAI_NULL_OBJECT_ID, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // create switch

        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;

        status = m_sairedis->create(SAI_OBJECT_TYPE_SWITCH, &m_switchId, SAI_NULL_OBJECT_ID, 1, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);
    }

    virtual void TearDown() override
    {
        SWSS_LOG_ENTER();

        // uninitialize SAI redis

        auto status = m_sairedis->uninitialize();
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        // stop syncd worker

        sendSyncdShutdownNotification();
        m_worker->join();
    }

protected:
    std::shared_ptr<std::thread> m_worker;
    std::shared_ptr<sairedis::Sai> m_sairedis;

    sai_object_id_t m_switchId = SAI_NULL_OBJECT_ID;
};

TEST_F(SyncdMlnxTest, portBulkAddRemove)
{
    const std::uint32_t portCount = 1;
    const std::uint32_t laneCount = 4;

    // Generate port config
    std::array<std::uint32_t, laneCount> laneList = { 1000, 1001, 1002, 1003 };

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrList;

    attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
    attr.value.u32list.count = static_cast<std::uint32_t>(laneList.size());
    attr.value.u32list.list = laneList.data();
    attrList.push_back(attr);

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = 1000;
    attrList.push_back(attr);

    std::array<std::uint32_t, portCount> attrCountList = { static_cast<std::uint32_t>(attrList.size()) };
    std::array<const sai_attribute_t*, portCount> attrPtrList = { attrList.data() };

    std::array<sai_object_id_t, portCount> oidList = { SAI_NULL_OBJECT_ID };
    std::array<sai_status_t, portCount> statusList = { SAI_STATUS_SUCCESS };

    // Validate port bulk add
    auto status = m_sairedis->bulkCreate(
        SAI_OBJECT_TYPE_PORT, m_switchId, portCount, attrCountList.data(), attrPtrList.data(),
        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
        oidList.data(), statusList.data()
    );
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    for (std::uint32_t i = 0; i < portCount; i++)
    {
        ASSERT_EQ(statusList.at(i), SAI_STATUS_SUCCESS);
    }

    // Validate port bulk remove
    status = m_sairedis->bulkRemove(
        SAI_OBJECT_TYPE_PORT, portCount, oidList.data(),
        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
        statusList.data()
    );
    ASSERT_EQ(status, SAI_STATUS_SUCCESS);

    for (std::uint32_t i = 0; i < portCount; i++)
    {
        ASSERT_EQ(statusList.at(i), SAI_STATUS_SUCCESS);
    }
}