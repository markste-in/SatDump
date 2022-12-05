#include "usrp_sdr.h"

void USRPSource::set_gains()
{
    if (!is_started)
        return;

    usrp_device->set_rx_gain(gain, channel);
    logger->debug("Set USRP gain to {:f}", gain);
}

void USRPSource::open_sdr()
{
    uhd::device_addrs_t devlist = uhd::device::find(uhd::device_addr_t());
    usrp_device = uhd::usrp::multi_usrp::make(devlist[d_sdr_id]);

    // uhd::meta_range_t master_clock_range = usrp_device->get_master_clock_rate_range();
    // usrp_device->set_master_clock_rate(master_clock_range.stop());

    uhd::usrp::subdev_spec_t sub_boards = usrp_device->get_rx_subdev_spec();
    channel_option_str = "";
    for (int i = 0; i < (int)sub_boards.size(); i++)
    {
        logger->trace("USRP has " + usrp_device->get_rx_subdev_name(i) + " in slot " + sub_boards[i].db_name);
        channel_option_str += usrp_device->get_rx_subdev_name(i) + " (" + sub_boards[i].db_name + ")" + '\0';
    }
}

void USRPSource::open_channel()
{
    if (channel >= (int)usrp_device->get_rx_num_channels())
        throw std::runtime_error("Channel " + std::to_string(channel) + " is invalid!");

    logger->info("Using USRP channel {:d}", channel);

    if (usrp_device->get_master_clock_rate_range().start() != usrp_device->get_master_clock_rate_range().stop())
        use_device_rates = true;

    if (use_device_rates)
    {
        // Get samplerates
        uhd::meta_range_t dev_samplerates = usrp_device->get_master_clock_rate_range();
        available_samplerates.clear();
        for (auto &sr : dev_samplerates)
        {
            if (sr.step() == 0 && sr.start() == sr.stop())
            {
                available_samplerates.push_back(sr.start());
            }
            else if (sr.step() == 0)
            {
                for (double s = std::max(sr.start(), 1e6); s < sr.stop(); s += 1e6)
                    available_samplerates.push_back(s);
                available_samplerates.push_back(sr.stop());
            }
            else
            {
                for (double s = sr.start(); s <= sr.stop(); s += sr.step())
                    available_samplerates.push_back(s);
            }
        }
    }
    else
    {
        // Get samplerates
        uhd::meta_range_t dev_samplerates = usrp_device->get_rx_rates(channel);
        available_samplerates.clear();
        for (auto &sr : dev_samplerates)
        {
            if (sr.step() == 0 || sr.start() == sr.stop())
            {
                available_samplerates.push_back(sr.start());
            }
            else
            {
                for (double s = sr.start(); s <= sr.stop(); s += sr.step())
                    available_samplerates.push_back(s);
            }
        }
    }

    // Init UI stuff
    samplerate_option_str = "";
    for (uint64_t samplerate : available_samplerates)
        samplerate_option_str += formatSamplerateToString(samplerate) + '\0';

    // Get gain range
    gain_range = usrp_device->get_rx_gain_range(channel);

    usrp_antennas = usrp_device->get_rx_antennas();
    antenna_option_str = "";
    for (int i = 0; i < (int)usrp_antennas.size(); i++)
    {
        antenna_option_str += usrp_antennas[i] + '\0';
    }
}

void USRPSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    channel = getValueOrDefault(d_settings["channel"], channel);
    antenna = getValueOrDefault(d_settings["antenna"], antenna);
    gain = getValueOrDefault(d_settings["gain"], gain);
    bit_depth = getValueOrDefault(d_settings["bit_depth"], bit_depth);

    if (bit_depth == 8)
        selected_bit_depth = 0;
    else if (bit_depth == 16)
        selected_bit_depth = 1;

    if (is_started)
    {
        set_gains();
    }
}

nlohmann::json USRPSource::get_settings()
{

    d_settings["channel"] = channel;
    d_settings["antenna"] = antenna;
    d_settings["gain"] = gain;
    d_settings["bit_depth"] = bit_depth;

    return d_settings;
}

void USRPSource::open()
{
    open_sdr();
    is_open = true;
    open_channel();
    usrp_device.reset();
}

void USRPSource::start()
{
    DSPSampleSource::start();
    open_sdr();
    open_channel();

    logger->debug("Set USRP samplerate to " + std::to_string(current_samplerate));
    if (use_device_rates)
        usrp_device->set_master_clock_rate(current_samplerate);
    usrp_device->set_rx_rate(current_samplerate, channel);
    usrp_device->set_rx_bandwidth(current_samplerate, channel);

    if (antenna >= (int)usrp_device->get_rx_antennas(channel).size())
        throw std::runtime_error("Antenna " + std::to_string(antenna) + " is invalid!");

    usrp_device->set_rx_antenna(usrp_antennas[antenna], channel);

    is_started = true;

    set_frequency(d_frequency);

    set_gains();

    uhd::stream_args_t sargs;
    sargs.channels.clear();
    sargs.channels.push_back(channel);
    sargs.cpu_format = "fc32";
    if (bit_depth == 8)
        sargs.otw_format = "sc8";
    else if (bit_depth == 16)
        sargs.otw_format = "sc16";

    usrp_streamer = usrp_device->get_rx_stream(sargs);
    usrp_streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    thread_should_run = true;
    work_thread = std::thread(&USRPSource::mainThread, this);
}

void USRPSource::stop()
{
    thread_should_run = false;
    logger->info("Waiting for the thread...");
    if (is_started)
        output_stream->stopWriter();
    if (work_thread.joinable())
        work_thread.join();
    logger->info("Thread stopped");
    if (is_started)
    {
        usrp_streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        usrp_streamer.reset();
        usrp_device.reset();
    }
    is_started = false;
}

void USRPSource::close()
{
    is_open = false;
}

void USRPSource::set_frequency(uint64_t frequency)
{
    if (is_started)
    {
        usrp_device->set_rx_freq(frequency, channel);
        logger->debug("Set USRP frequency to {:d}", frequency);
    }
    DSPSampleSource::set_frequency(frequency);
}

void USRPSource::drawControlUI()
{
    if (is_started)
        style::beginDisabled();

    if (ImGui::Combo("Channel", &channel, channel_option_str.c_str()))
    {
        open_sdr();
        open_channel();
        usrp_streamer.reset();
        usrp_device.reset();
    }

    ImGui::Combo("Antenna", &antenna, antenna_option_str.c_str());

    ImGui::Combo("Samplerate", &selected_samplerate, samplerate_option_str.c_str());
    current_samplerate = available_samplerates[selected_samplerate];

    if (ImGui::Combo("Bit depth", &selected_bit_depth, "8-bits\0"
                                                       "16-bits\0"))
    {
        if (selected_bit_depth == 0)
            bit_depth = 8;
        else if (selected_bit_depth == 1)
            bit_depth = 16;
    }

    if (is_started)
        style::endDisabled();

    // Gain settings
    if (ImGui::SliderFloat("Gain", &gain, gain_range.start(), gain_range.stop()))
        set_gains();
}

void USRPSource::set_samplerate(uint64_t samplerate)
{
    for (int i = 0; i < (int)available_samplerates.size(); i++)
    {
        if (samplerate == available_samplerates[i])
        {
            selected_samplerate = i;
            current_samplerate = samplerate;
            return;
        }
    }

    throw std::runtime_error("Unspported samplerate : " + std::to_string(samplerate) + "!");
}

uint64_t USRPSource::get_samplerate()
{
    return current_samplerate;
}

std::vector<dsp::SourceDescriptor> USRPSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    uhd::device_addrs_t devlist = uhd::device::find(uhd::device_addr_t());

    uint64_t i = 0;
    for (const uhd::device_addr_t &dev : devlist)
    {
        std::string type = dev.has_key("product") ? dev["product"] : dev["type"];
        results.push_back({"usrp", "USRP " + type + " " + dev["serial"], i});
        i++;
    }

    return results;
}