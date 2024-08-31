#include "Resolver.h"

#include "../PacketManip/AntiAim/AntiAim.h"
#include "../Backtrack/Backtrack.h"

static std::vector<float> vYawRotations{ 0.0f, 180.0f, 90.0f, -90.0f };

void PResolver::UpdateSniperDots()
{
    mSniperDots.clear();
    for (auto& pEntity : H::Entities.GetGroup(EGroupType::MISC_DOTS))
    {
        if (auto pOwner = pEntity->m_hOwnerEntity().Get())
            mSniperDots[pOwner] = pEntity->m_vecOrigin();
    }
}

std::optional<float> PResolver::GetPitchForSniperDot(CTFPlayer* pEntity)
{
    if (mSniperDots.contains(pEntity))
    {
        const Vec3 vOrigin = mSniperDots[pEntity];
        const Vec3 vEyeOrigin = pEntity->GetEyePosition();
        const Vec3 vDelta = vOrigin - vEyeOrigin;
        Vec3 vAngles;
        Math::VectorAngles(vDelta, vAngles);
        return vAngles.x;
    }
    return std::nullopt;
}

std::optional<float> PResolver::PredictBaseYaw(CTFPlayer* pLocal, CTFPlayer* pEntity)
{
    if (I::GlobalVars->tickcount - mResolverData[pEntity].pLastFireAngles.first.first > 66 || !mResolverData[pEntity].pLastFireAngles.first.first)
    {   // staleness & validity check
        if (!pLocal || !pLocal->IsAlive() || pLocal->IsAGhost())
            return std::nullopt;
        return Math::CalcAngle(pEntity->m_vecOrigin(), pLocal->m_vecOrigin()).y;
    }

    bool bFound = false;
    float flSmallestAngleTo = 0.f; float flSmallestFovTo = 360.f;
    for (auto pTarget : H::Entities.GetGroup(EGroupType::PLAYERS_ALL))
    {
        auto pPlayer = pTarget->As<CTFPlayer>();
        if (!pPlayer || pPlayer->IsAGhost() || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pEntity->m_iTeamNum())
            continue;

        const Vec3 vAngleTo = Math::CalcAngle(pEntity->m_vecOrigin(), pPlayer->m_vecOrigin());
        const float flFOVTo = Math::CalcFov(mResolverData[pEntity].pLastFireAngles.second, vAngleTo);

        if (flFOVTo < flSmallestFovTo)
        {
            bFound = true;
            flSmallestAngleTo = vAngleTo.y;
            flSmallestFovTo = flFOVTo;
        }
    }

    if (!bFound)
        return std::nullopt;

    return flSmallestAngleTo;
}

bool PResolver::ShouldRun(CTFPlayer* pLocal)
{
    return Vars::AntiHack::Resolver::Resolver.Value && pLocal && pLocal->IsAlive() && !pLocal->IsAGhost() && G::WeaponType == EWeaponType::HITSCAN;
}

bool PResolver::ShouldRunEntity(CTFPlayer* pEntity)
{
    if (!pEntity->OnSolid() && Vars::AntiHack::Resolver::IgnoreAirborne.Value)
        return false;
    if (!pEntity->IsAlive() || pEntity->IsAGhost() || pEntity->IsTaunting())
        return false;

    //if (pEntity->GetSimulationTime() == pEntity->GetOldSimulationTime())
    //    return false; // last networked angles are the same as these, no need to change them
    return true;
}

bool PResolver::KeepOnShot(CTFPlayer* pEntity)
{
    if (abs(I::GlobalVars->tickcount - mResolverData[pEntity].pLastFireAngles.first.first) < 2)
        return true;
    if (mResolverData[pEntity].pLastFireAngles.first.second)
        return true; // this person has not unchoked since shooting
    return false;
}

bool PResolver::IsOnShotPitchReliable(const float flPitch)
{
    if (flPitch > 180)
        return flPitch > 273.f;
    else
        return flPitch < 87.f;
}

float PResolver::GetRealPitch(const float flPitch)
{
    if (flPitch < 157.5f)
        return 89.f;
    else
        return -89.f;
}

void PResolver::SetAngles(const Vec3 vAngles, CTFPlayer* pEntity)
{
    if (auto pAnimState = pEntity->GetAnimState())
    {
        pEntity->m_angEyeAnglesX() = vAngles.x;
        pAnimState->m_flCurrentFeetYaw = vAngles.y;
        pAnimState->m_flGoalFeetYaw = vAngles.y;
        pAnimState->Update(vAngles.y, vAngles.x);
    }
}

int PResolver::GetPitchMode(CTFPlayer* pEntity)
{
    PlayerInfo_t pi{};
    if (!I::EngineClient->GetPlayerInfo(pEntity->entindex(), &pi))
        return 0;
    return mResolverMode[pi.friendsID].first;
}

int PResolver::GetYawMode(CTFPlayer* pEntity)
{
    PlayerInfo_t pi{};
    if (!I::EngineClient->GetPlayerInfo(pEntity->entindex(), &pi))
        return 0;
    return mResolverMode[pi.friendsID].second;
}

void PResolver::OnDormancy(CTFPlayer* pEntity)
{
    mResolverData[pEntity].pLastSniperPitch = { 0, 0.f };
    mResolverData[pEntity].flPitchNoise = 0.f;
    mResolverData[pEntity].iPitchNoiseSteps = 0;
    mResolverData[pEntity].pLastFireAngles = { {0, false}, {} };
    mResolverData[pEntity].vOriginalAngles = {};
}

void PResolver::Aimbot(CTFPlayer* pEntity, const bool bHeadshot)
{
    if (abs(I::GlobalVars->tickcount - pWaiting.first) < 66)
        return;

    auto pNetChan = I::EngineClient->GetNetChannelInfo();
    if (!pNetChan)
        return;

    const int iDelay = 6 + TIME_TO_TICKS(G::Lerp + pNetChan->GetLatency(FLOW_INCOMING) + pNetChan->GetLatency(FLOW_OUTGOING));
    pWaiting = { I::GlobalVars->tickcount + iDelay, {pEntity, bHeadshot} };
}

void PResolver::FrameStageNotify(CTFPlayer* pLocal)
{
    if (!ShouldRun(pLocal))
        return;

    UpdateSniperDots();

    for (auto& pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_ALL))
    {
        auto pPlayer = pEntity->As<CTFPlayer>();
        if (pPlayer->entindex() == I::EngineClient->GetLocalPlayer())
            continue;

        if (pPlayer->IsDormant())
        {
            OnDormancy(pPlayer);
            continue;
        }

        mResolverData[pPlayer].vOriginalAngles = { pPlayer->m_angEyeAnglesX(), pPlayer->m_angEyeAnglesY() };

        if (abs(I::GlobalVars->tickcount - mResolverData[pPlayer].pLastFireAngles.first.first) >= 2)
            mResolverData[pPlayer].pLastFireAngles.first.second = (pPlayer->m_flSimulationTime() == pPlayer->m_flOldSimulationTime()) ? mResolverData[pPlayer].pLastFireAngles.first.second : false;

        if (!ShouldRunEntity(pPlayer))
            continue;
        if (KeepOnShot(pPlayer))
        {
            SetAngles(mResolverData[pPlayer].pLastFireAngles.second, pPlayer);
            continue;
        }

        Vec3 vAdjustedAngle = pPlayer->GetEyeAngles();

        if (std::optional<float> flPitch = GetPitchForSniperDot(pPlayer))
        {
            vAdjustedAngle.x = flPitch.value();

            // get noise
            if (mResolverData[pPlayer].pLastSniperPitch.first)
            {
                const float flNoise = mResolverData[pPlayer].pLastSniperPitch.second - flPitch.value();
                mResolverData[pPlayer].flPitchNoise *= mResolverData[pPlayer].iPitchNoiseSteps;
                mResolverData[pPlayer].flPitchNoise += flNoise;
                mResolverData[pPlayer].iPitchNoiseSteps++;
                mResolverData[pPlayer].flPitchNoise /= mResolverData[pPlayer].iPitchNoiseSteps;
            }

            mResolverData[pPlayer].pLastSniperPitch = { I::GlobalVars->tickcount, flPitch.value() };
        }
        else if (I::GlobalVars->tickcount - mResolverData[pPlayer].pLastSniperPitch.first < 66 && mResolverData[pPlayer].flPitchNoise < 5.f)
            vAdjustedAngle.x = mResolverData[pPlayer].pLastSniperPitch.second;
        else
        {
            switch (GetPitchMode(pPlayer))
            {
            case 0: break;
            case 1: vAdjustedAngle.x = -89.f; break;                // up
            case 2: vAdjustedAngle.x = 89.f; break;                 // down
            case 3: vAdjustedAngle.x = 0.f; break;                  // zero
            case 4:                                                    // random
                vAdjustedAngle.x = ((rand() % 3) - 1) * 89.f;
                break;
            }
        }

        if (vAdjustedAngle.x > 180.f)
            vAdjustedAngle.x -= 360.f;

        const std::optional<float> flBaseYaw = PredictBaseYaw(pLocal, pPlayer);
        if (!flBaseYaw.has_value())
            continue;

        if (flBaseYaw.value() > 180.f)
            vAdjustedAngle.y = flBaseYaw.value() - 360.f;
        else
            vAdjustedAngle.y = flBaseYaw.value();

        vAdjustedAngle.y = (vAdjustedAngle.y);

        if (GetYawMode(pPlayer) != 0)
        {
            if (GetYawMode(pPlayer) == 1)
                vAdjustedAngle.y += 180.f;
            else if (GetYawMode(pPlayer) == 2)
                vAdjustedAngle.y += 90.f;
            else if (GetYawMode(pPlayer) == 3)
                vAdjustedAngle.y -= 90.f;
        }

        vAdjustedAngle.y =(vAdjustedAngle.y);

        SetAngles(vAdjustedAngle, pPlayer);
    }
}

void PResolver::CreateMove()
{
    if (!pWaiting.first || abs(I::GlobalVars->tickcount - pWaiting.first) > 66)
        pWaiting.second.first = nullptr;

    if (pWaiting.second.first && pWaiting.second.first->IsAlive())
    {
        pWaiting.second.first->m_angEyeAnglesX() = pWaiting.second.second ? 89.f : -89.f;
        pWaiting.second.first->m_angEyeAnglesY() = (pWaiting.second.first->m_angEyeAnglesY() + (pWaiting.second.second ? 180.f : 0.f));
    }
}

void PResolver::FXFireBullet()
{
    if (pWaiting.first)
    {
        const float flAimbotAng = pWaiting.second.second ? 89.f : -89.f;
        pWaiting.second.first->m_angEyeAnglesX() = flAimbotAng;
        pWaiting.second.first->m_angEyeAnglesY() = (pWaiting.second.first->m_angEyeAnglesY() + (pWaiting.second.second ? 180.f : 0.f));
    }
}
