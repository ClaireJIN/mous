#include "Player.h"

#include <unistd.h>
#include <assert.h>

#include <iostream>
#include <algorithm>
#include <future>
using namespace std;

#include <scx/Conv.hpp>
#include <scx/FileHelper.hpp>
using namespace scx;

#include <plugin/IDecoder.h>
#include <plugin/IRenderer.h>
#include <core/IPluginAgent.h>
using namespace mous;

IPlayer* IPlayer::Create()
{
    return new Player;
}

void IPlayer::Free(IPlayer* player)
{
    if (player != nullptr)
        delete player;
}

Player::Player()
{
    m_UnitBuffers.AllocBuffer(5);

    const auto& f1 = std::bind(&Player::ThDecoder, this);
    m_ThreadForDecoder = std::thread(f1);

    const auto& f2 = std::bind(&Player::ThRenderer, this);
    m_ThreadForRenderer = std::thread(f2);
}

Player::~Player()
{
    Close();

    m_StopDecoder = true;
    m_StopRenderer = true;
    m_SemWakeDecoder.Post();
    m_SemWakeRenderer.Post();

    if (m_ThreadForDecoder.joinable())
        m_ThreadForDecoder.join();
    if (m_ThreadForRenderer.joinable())
        m_ThreadForRenderer.join();

    m_UnitBuffers.ClearBuffer();

    UnregisterAll();
}

EmPlayerStatus Player::Status() const
{
    return m_Status;
}

void Player::RegisterDecoderPlugin(const IPluginAgent* pAgent)
{
    if (pAgent->Type() == PluginType::Decoder)
        AddDecoderPlugin(pAgent);
}

void Player::RegisterDecoderPlugin(vector<const IPluginAgent*>& agents)
{
    for (const auto agent: agents) {
        RegisterDecoderPlugin(agent);
    }
}

void Player::RegisterRendererPlugin(const IPluginAgent* pAgent)
{
    if (pAgent->Type() == PluginType::Renderer)
        SetRendererPlugin(pAgent);
}

void Player::UnregisterPlugin(const IPluginAgent* pAgent)
{
    switch (pAgent->Type()) {
        case PluginType::Decoder:
            RemoveDecoderPlugin(pAgent);
            break;

        case PluginType::Renderer:
            UnsetRendererPlugin(pAgent);
            break;

        default:
            break;
    }
}

void Player::UnregisterPlugin(vector<const IPluginAgent*>& agents)
{
    for (const auto agent: agents) {
        UnregisterPlugin(agent);
    }
}

void Player::AddDecoderPlugin(const IPluginAgent* pAgent)
{
    // create Decoder & get suffix
    IDecoder* pDecoder = (IDecoder*)pAgent->CreateObject();
    const vector<string>& list = pDecoder->FileSuffix();

    // try add
    bool usedAtLeastOnce = false;
    for (const string& item: list) {
        const string& suffix = ToLower(item);
        auto iter = m_DecoderPluginMap.find(suffix);
        if (iter == m_DecoderPluginMap.end()) {
            m_DecoderPluginMap.emplace(suffix, DecoderPluginNode { pAgent, pDecoder });
            usedAtLeastOnce = true;
        }
    }

    // clear if not used
    if (!usedAtLeastOnce) {
        pAgent->FreeObject(pDecoder);
    }
}

void Player::RemoveDecoderPlugin(const IPluginAgent* pAgent)
{
    // get suffix
    IDecoder* pDecoder = (IDecoder*)pAgent->CreateObject();
    const vector<string>& list = pDecoder->FileSuffix();
    pAgent->FreeObject(pDecoder);

    // find plugin
    bool freedOnce = false;
    for (const string& item: list) {
        const string& suffix = ToLower(item);
        auto iter = m_DecoderPluginMap.find(suffix);
        if (iter != m_DecoderPluginMap.end()) {
            const DecoderPluginNode& node = iter->second;
            if (node.agent == pAgent) {
                if (!freedOnce) {
                    if (node.decoder == m_Decoder) {
                        Close();
                    }
                    pAgent->FreeObject(node.decoder);
                    freedOnce = true;
                }
                m_DecoderPluginMap.erase(iter);
            }
        }
    }
}

void Player::SetRendererPlugin(const IPluginAgent* pAgent)
{
    if (pAgent == nullptr || m_RendererPlugin != nullptr)
        return;

    m_RendererPlugin = pAgent;
    m_Renderer = (IRenderer*)pAgent->CreateObject();
    m_Renderer->Open();
}

void Player::UnsetRendererPlugin(const IPluginAgent* pAgent)
{
    if (pAgent != m_RendererPlugin || m_RendererPlugin == nullptr)
        return;

    m_Renderer->Close();
    m_RendererPlugin->FreeObject(m_Renderer);
    m_Renderer = nullptr;
    m_RendererPlugin = nullptr;
}

void Player::UnregisterAll()
{
    while (!m_DecoderPluginMap.empty()) {
        auto iter = m_DecoderPluginMap.begin();
        RemoveDecoderPlugin(iter->second.agent);
    }

    UnsetRendererPlugin(m_RendererPlugin);
}

vector<string> Player::SupportedSuffixes() const
{
    vector<string> list;
    list.reserve(m_DecoderPluginMap.size());
    for (const auto& entry: m_DecoderPluginMap)
        list.push_back(entry.first);
    return list;
}

int Player::BufferCount() const
{
    return m_UnitBuffers.BufferCount();
}

void Player::SetBufferCount(int count)
{
    m_UnitBuffers.ClearBuffer();
    m_UnitBuffers.AllocBuffer(count);
}

int Player::Volume() const
{
    return m_Renderer != nullptr ? m_Renderer->VolumeLevel() : -1;
}

void Player::SetVolume(int level)
{
    if (m_Renderer != nullptr)
        m_Renderer->SetVolumeLevel(level);
}

EmErrorCode Player::Open(const string& path)
{
    string suffix = ToLower(FileHelper::FileSuffix(path));
    //cout << "Suffix:" << suffix << endl;
    auto iter = m_DecoderPluginMap.find(suffix);
    if (iter != m_DecoderPluginMap.end()) {
        m_Decoder = iter->second.decoder;
    } else {
        return ErrorCode::PlayerNoDecoder;
    }

    if (m_Renderer == nullptr)
        return ErrorCode::PlayerNoRenderer;

    EmErrorCode err = m_Decoder->Open(path);
    if (err != ErrorCode::Ok) {
        //cout << "FATAL: failed to open!" << endl;
        return err;
    } else {
        m_DecodeFile = path;
    }

    uint32_t maxBytesPerUnit = m_Decoder->MaxBytesPerUnit();
    for (size_t i = 0; i < m_UnitBuffers.BufferCount(); ++i) {
        UnitBuffer* buf = m_UnitBuffers.RawItemAt(i);
        buf->used = 0;
        if (buf->max < maxBytesPerUnit) {
            if (buf->data != nullptr) {
                delete[] buf->data;
                //cout << "free unit buf:" << buf->max << endl;
            }
            buf->data = new char[maxBytesPerUnit];
            buf->max = maxBytesPerUnit;
            //cout << "alloc unit buf:" << buf->max << endl;
        }
    }
    //cout << "unit buf size:" << maxBytesPerUnit << endl;

    m_UnitPerMs = (double)m_Decoder->UnitCount() / m_Decoder->Duration();

    int32_t channels = m_Decoder->Channels();
    int32_t samleRate = m_Decoder->SampleRate();
    int32_t bitsPerSamle = m_Decoder->BitsPerSample();
    //cout << "channels:" << channels << endl;
    //cout << "samleRate:" << samleRate << endl;
    //cout << "bitsPerSamle:" << bitsPerSamle << endl;
    err = m_Renderer->Setup(channels, samleRate, bitsPerSamle);
    if (err != ErrorCode::Ok) {
        cout << "FATAL: failed to set renderer:" << err << endl;
        cout << "       channels:" << channels << endl;
        cout << "       samleRate:" << samleRate << endl;
        cout << "       bitsPerSamle:" << bitsPerSamle << endl;
        return err;
    }

    m_Status = PlayerStatus::Stopped;

    return err;
}

void Player::Close()
{
    if (m_Status == PlayerStatus::Closed)
        return;

    Pause();

    m_Decoder->Close();
    m_Decoder = nullptr;
    m_DecodeFile.clear();

    m_Status = PlayerStatus::Closed;
}

string Player::FileName() const
{
    return m_DecodeFile;
}

void Player::Play()
{
    uint64_t beg = 0;
    uint64_t end = m_Decoder->UnitCount();
    PlayRange(beg, end);
}

void Player::Play(uint64_t msBegin, uint64_t msEnd)
{
    const uint64_t total = m_Decoder->UnitCount();

    uint64_t beg = 0;
    uint64_t end = 0;

    beg = m_UnitPerMs * msBegin;
    if (beg > total)
        beg = total;

    if (msEnd != (uint64_t)-1) {
        end = m_UnitPerMs * msEnd;
        if (end > total)
            end = total;
    } else {
        end = total;
    }

    //cout << "begin:" << beg << endl;
    //cout << "end:" << end << endl;
    //cout << "total:" << total << endl;

    PlayRange(beg, end);
}

void Player::PlayRange(uint64_t beg, uint64_t end)
{
    m_UnitBeg = beg;
    m_UnitEnd = end;

    m_DecoderIndex = m_UnitBeg;
    m_RendererIndex = m_UnitBeg;

    m_Decoder->SetUnitIndex(m_UnitBeg);

    m_UnitBuffers.ResetPV();

    m_SuspendRenderer = false;
    m_SemWakeRenderer.Post();
    m_SuspendDecoder = false;
    m_SemWakeDecoder.Post();
    m_SemRendererBegin.Wait();
    m_SemDecoderBegin.Wait();

    m_Status = PlayerStatus::Playing;
}

void Player::Pause()
{
    if (m_Status == PlayerStatus::Paused)
        return;

    // suspend renderer
    if (!m_SuspendRenderer) {
        m_SuspendRenderer = true;
        m_UnitBuffers.RecycleFree();
    }
    m_SemRendererEnd.Wait();

    // suspend decoder
    if (!m_SuspendDecoder) {
        m_SuspendDecoder = true;
        m_UnitBuffers.RecycleData();
    }
    m_SemDecoderEnd.Wait();

    m_UnitBuffers.ResetPV();

    m_Status = PlayerStatus::Paused;
}

void Player::Resume()
{
    m_DecoderIndex = m_RendererIndex;
    m_Decoder->SetUnitIndex(m_DecoderIndex);

    m_UnitBuffers.ResetPV();

    // resume renderer & decoder
    m_SuspendRenderer = false;
    m_SemWakeRenderer.Post();
    m_SuspendDecoder = false;
    m_SemWakeDecoder.Post();
    m_SemRendererBegin.Wait();
    m_SemDecoderBegin.Wait();

    m_Status = PlayerStatus::Playing;
}

void Player::SeekTime(uint64_t msPos)
{
    switch (m_Status) {
        case PlayerStatus::Playing:
            Pause();
            DoSeekTime(msPos);
            Resume();
            break;

        case PlayerStatus::Paused:
        case PlayerStatus::Stopped:
            DoSeekTime(msPos);
            break;

        default:
            break;
    }
}

void Player::SeekPercent(double percent)
{
    uint64_t unit = m_UnitBeg + (m_UnitEnd - m_UnitBeg) * percent;

    switch (m_Status) {
        case PlayerStatus::Playing:
            Pause();
            DoSeekUnit(unit);
            Resume();
            break;

        case PlayerStatus::Paused:
        case PlayerStatus::Stopped:
            DoSeekUnit(unit);
            break;

        default:
            break;
    }
}

void Player::DoSeekTime(uint64_t msPos)
{
    uint64_t unitPos = std::min((uint64_t)(m_UnitPerMs*msPos), m_Decoder->UnitCount());
    DoSeekUnit(unitPos);
}

void Player::DoSeekUnit(uint64_t unit)
{
    if (unit < m_UnitBeg) 
        unit = m_UnitBeg;
    else if (unit > m_UnitEnd)
        unit = m_UnitEnd;

    m_Decoder->SetUnitIndex(unit);

    m_DecoderIndex = unit;
    m_RendererIndex = unit;
}

void Player::PauseDecoder()
{
    //cout << "data:" << m_UnitBuffers.DataCount() << endl;
    //cout << "free:" << m_UnitBuffers.FreeCount() << endl;

    if (!m_PauseDecoder) {
        m_PauseDecoder = true;
    }
    m_SemDecoderEnd.Wait();

    m_Decoder->Close();
}

void Player::ResumeDecoder()
{
    //cout << "data:" << m_UnitBuffers.DataCount() << endl;
    //cout << "free:" << m_UnitBuffers.FreeCount() << endl;

    m_Decoder->Open(m_DecodeFile);
    m_Decoder->SetUnitIndex(m_DecoderIndex);

    m_PauseDecoder = false;
    m_SemWakeDecoder.Post();
    m_SemDecoderBegin.Wait();
}

int32_t Player::BitRate() const
{
    return (m_Decoder != nullptr) ? m_Decoder->BitRate() : -1;
}

int32_t Player::SamleRate() const
{
    return (m_Decoder != nullptr) ? m_Decoder->SampleRate() : -1;
}

uint64_t Player::Duration() const
{
    return m_Decoder->Duration();
}

uint64_t Player::RangeBegin() const
{
    return m_UnitBeg / m_UnitPerMs;
}

uint64_t Player::RangeEnd() const
{
    return m_UnitEnd / m_UnitPerMs;
}

uint64_t Player::RangeDuration() const
{
    return (m_UnitEnd - m_UnitBeg) / m_UnitPerMs;
}

uint64_t Player::OffsetMs() const
{
    return CurrentMs() - RangeBegin();
}

uint64_t Player::CurrentMs() const
{
    return m_RendererIndex / m_UnitPerMs;
}

EmAudioMode Player::AudioMode() const
{
    return (m_Decoder != nullptr) ? m_Decoder->AudioMode() : AudioMode::None;
}

std::vector<PluginOption> Player::DecoderPluginOption() const
{
    std::vector<PluginOption> list;
    PluginOption optionItem;

    for (auto entry: m_DecoderPluginMap) {
        const DecoderPluginNode& node = entry.second;
        const vector<const BaseOption*>& opt = node.decoder->Options();
        if (!opt.empty()) {
            optionItem.pluginType = node.agent->Type();
            optionItem.pluginInfo = node.agent->Info();
            list.push_back(optionItem);
        }
    }

    return list;
}

PluginOption Player::RendererPluginOption() const
{
    PluginOption option;
    if (m_RendererPlugin != nullptr) {
        option.pluginType = m_RendererPlugin->Type();
        option.pluginInfo = m_RendererPlugin->Info();
        option.options = m_Renderer->Options();
    } else {
        option.pluginType = PluginType::None; 
        option.pluginInfo = nullptr;
        option.options.clear();
    }
    return option;
}

Signal<void (void)>* Player::SigFinished()
{
    return &m_SigFinished;
}

void Player::ThDecoder()
{
    while (true) {
        m_SemWakeDecoder.Wait();
        if (m_StopDecoder)
            break;

        m_SemDecoderBegin.Clear();
        m_SemDecoderEnd.Clear();

        m_SemDecoderBegin.Post();

        for (UnitBuffer* buf = nullptr; ; ) {
            if (m_PauseDecoder)
                break;

            buf = m_UnitBuffers.TakeFree();
            if (m_SuspendDecoder)
                break;

            assert(buf != nullptr);
            assert(buf->data != nullptr);

            m_Decoder->DecodeUnit(buf->data, buf->used, buf->unitCount);
            m_DecoderIndex += buf->unitCount;
            m_UnitBuffers.RecycleFree();

            if (m_DecoderIndex >= m_UnitEnd) {
                m_SuspendDecoder = true;
                break;
            }
        }

        m_SemDecoderEnd.Post();
    }
}

void Player::ThRenderer()
{
    while (true) {
        m_SemWakeRenderer.Wait();
        if (m_StopRenderer)
            break;

        m_SemRendererBegin.Clear();
        m_SemRendererEnd.Clear();

        m_SemRendererBegin.Post();

        for (UnitBuffer* buf = nullptr; ; ) {
            buf = m_UnitBuffers.TakeData();
            if (m_SuspendRenderer)
                break;

            assert(buf != nullptr);
            assert(buf->data != nullptr);

            // avoid busy write
            if (m_Renderer->Write(buf->data, buf->used) != ErrorCode::Ok)
                ::usleep(10*1000);
            m_RendererIndex += buf->unitCount;
            m_UnitBuffers.RecycleData();

            if (m_RendererIndex >= m_UnitEnd) {
                m_SuspendRenderer = true;
                break;
            }
        }

        m_SemRendererEnd.Post();

        if (m_RendererIndex >= m_UnitEnd) {
            m_Status = PlayerStatus::Stopped;
            std::thread([this]() { m_SigFinished(); }).detach();
        }
    }
}
