#include "stdafx.h"
#include <iostream>
#include "Scheduler.h"
#include "Logger.h"
#include "Helper.h"
#include "SQLHelper.h"
#include "mainworker.h"
#include "WebServer.h"
#include "HTMLSanitizer.h"
#include "../webserver/cWebem.h"
#include <json/json.h>
#include "boost/date_time/gregorian/gregorian.hpp"
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#define round(a) ( int ) ( a + .5 )

CScheduler::CScheduler()
{
	m_tSunRise = 0;
	m_tSunSet = 0;
	m_tSunAtSouth = 0;
	m_tCivTwStart = 0;
	m_tCivTwEnd = 0;
	m_tNautTwStart = 0;
	m_tNautTwEnd = 0;
	m_tAstTwStart = 0;
	m_tAstTwEnd = 0;
	srand((int)mytime(nullptr));
}

void CScheduler::StartScheduler()
{
	m_thread = std::make_shared<std::thread>([this] { Do_Work(); });
	SetThreadName(m_thread->native_handle(), "Scheduler");
}

void CScheduler::StopScheduler()
{
	if (m_thread)
	{
		RequestStop();
		m_thread->join();
		m_thread.reset();
	}
}

std::vector<tScheduleItem> CScheduler::GetScheduleItems()
{
	std::lock_guard<std::mutex> l(m_mutex);
	return std::vector<tScheduleItem>{ m_scheduleitems };
}

void CScheduler::ReloadSchedules()
{
	std::lock_guard<std::mutex> l(m_mutex);
	m_scheduleitems.clear();

	std::vector<std::vector<std::string> > result;

	time_t atime = mytime(nullptr);
	struct tm ltime;
	localtime_r(&atime, &ltime);

	//Add Device Timers
	result = m_sql.safe_query(
		"SELECT T1.DeviceRowID, T1.Time, T1.Type, T1.Cmd, T1.Level, T1.Days, T2.Name,"
		" T2.Used, T1.UseRandomness, T1.Color, T1.[Date], T1.MDay, T1.Month, T1.Occurence, T1.ID"
		" FROM Timers as T1, DeviceStatus as T2"
		" WHERE ((T1.Active == 1) AND ((T1.TimerPlan == %d) OR (T1.TimerPlan == 9999)) AND (T2.ID == T1.DeviceRowID))"
		" ORDER BY T1.ID",
		m_sql.m_ActiveTimerPlan);
	if (!result.empty())
	{
		for (const auto& sd : result)
		{
			bool bDUsed = (atoi(sd[7].c_str()) != 0);

			if (bDUsed == true)
			{
				tScheduleItem titem;

				titem.bEnabled = true;
				titem.bIsScene = false;
				titem.bIsThermostat = false;

				_eTimerType timerType = (_eTimerType)atoi(sd[2].c_str());
				titem.RowID = std::stoull(sd[0]);
				titem.TimerID = std::stoull(sd[14]);
				titem.startHour = (unsigned char)atoi(sd[1].substr(0, 2).c_str());
				titem.startMin = (unsigned char)atoi(sd[1].substr(3, 2).c_str());
				titem.startTime = 0;
				titem.timerType = timerType;
				titem.timerCmd = (_eTimerCommand)atoi(sd[3].c_str());
				titem.Level = (unsigned char)atoi(sd[4].c_str());
				titem.bUseRandomness = (atoi(sd[8].c_str()) != 0);
				titem.Color = _tColor(sd[9]);
				titem.MDay = 0;
				titem.Month = 0;
				titem.Occurence = 0;

				if (timerType == TTYPE_FIXEDDATETIME)
				{
					std::string sdate = sd[10];
					if (sdate.size() != 10)
						continue; //invalid
					titem.startYear = (unsigned short)atoi(sdate.substr(0, 4).c_str());
					titem.startMonth = (unsigned char)atoi(sdate.substr(5, 2).c_str());
					titem.startDay = (unsigned char)atoi(sdate.substr(8, 2).c_str());
				}
				else if (timerType == TTYPE_MONTHLY)
				{
					std::string smday = sd[11];
					if (smday == "0")
						continue; //invalid
					titem.MDay = atoi(smday.c_str());
				}
				else if (timerType == TTYPE_MONTHLY_WD)
				{
					std::string socc = sd[13];
					if (socc == "0")
						continue; //invalid
					titem.Occurence = atoi(socc.c_str());
				}
				else if (timerType == TTYPE_YEARLY)
				{
					std::string smday = sd[11];
					std::string smonth = sd[12];
					if ((smday == "0") || (smonth == "0"))
						continue; //invalid
					titem.MDay = atoi(smday.c_str());
					titem.Month = atoi(smonth.c_str());
				}
				else if (timerType == TTYPE_YEARLY_WD)
				{
					std::string smonth = sd[12];
					std::string socc = sd[13];
					if ((smonth == "0") || (socc == "0"))
						continue; //invalid
					titem.Month = atoi(smonth.c_str());
					titem.Occurence = atoi(socc.c_str());
				}

				if ((titem.timerCmd == TCMD_ON) && (titem.Level == 0))
				{
					titem.Level = 100;
				}
				titem.Days = atoi(sd[5].c_str());
				titem.DeviceName = sd[6];

				if (AdjustScheduleItem(&titem, false) == true)
					m_scheduleitems.push_back(titem);
			}
			else
			{
				//not used? delete it
				m_sql.safe_query("DELETE FROM Timers WHERE (DeviceRowID == '%q')",
					sd[0].c_str());
			}
		}
	}

	//Add Scene Timers
	result = m_sql.safe_query(
		"SELECT T1.SceneRowID, T1.Time, T1.Type, T1.Cmd, T1.Level, T1.Days, T2.Name,"
		" T1.UseRandomness, T1.[Date], T1.MDay, T1.Month, T1.Occurence, T1.ID"
		" FROM SceneTimers as T1, Scenes as T2"
		" WHERE ((T1.Active == 1) AND ((T1.TimerPlan == %d) OR (T1.TimerPlan == 9999)) AND (T2.ID == T1.SceneRowID))"
		" ORDER BY T1.ID",
		m_sql.m_ActiveTimerPlan);
	if (!result.empty())
	{
		for (const auto& sd : result)
		{
			tScheduleItem titem;

			titem.bEnabled = true;
			titem.bIsScene = true;
			titem.bIsThermostat = false;
			titem.MDay = 0;
			titem.Month = 0;
			titem.Occurence = 0;

			{
				std::stringstream s_str(sd[0]);
				s_str >> titem.RowID; }
			{
				std::stringstream s_str(sd[12]);
				s_str >> titem.TimerID; }

			_eTimerType timerType = (_eTimerType)atoi(sd[2].c_str());

			if (timerType == TTYPE_FIXEDDATETIME)
			{
				std::string sdate = sd[8];
				if (sdate.size() != 10)
					continue; //invalid
				titem.startYear = (unsigned short)atoi(sdate.substr(0, 4).c_str());
				titem.startMonth = (unsigned char)atoi(sdate.substr(5, 2).c_str());
				titem.startDay = (unsigned char)atoi(sdate.substr(8, 2).c_str());
			}
			else if (timerType == TTYPE_MONTHLY)
			{
				std::string smday = sd[9];
				if (smday == "0")
					continue; //invalid
				titem.MDay = atoi(smday.c_str());
			}
			else if (timerType == TTYPE_MONTHLY_WD)
			{
				std::string socc = sd[11];
				if (socc == "0")
					continue; //invalid
				titem.Occurence = atoi(socc.c_str());
			}
			else if (timerType == TTYPE_YEARLY)
			{
				std::string smday = sd[9];
				std::string smonth = sd[10];
				if ((smday == "0") || (smonth == "0"))
					continue; //invalid
				titem.MDay = atoi(smday.c_str());
				titem.Month = atoi(smonth.c_str());
			}
			else if (timerType == TTYPE_YEARLY_WD)
			{
				std::string smonth = sd[10];
				std::string socc = sd[11];
				if ((smonth == "0") || (socc == "0"))
					continue; //invalid
				titem.Month = atoi(smonth.c_str());
				titem.Occurence = atoi(socc.c_str());
			}

			titem.startHour = (unsigned char)atoi(sd[1].substr(0, 2).c_str());
			titem.startMin = (unsigned char)atoi(sd[1].substr(3, 2).c_str());
			titem.startTime = 0;
			titem.timerType = timerType;
			titem.timerCmd = (_eTimerCommand)atoi(sd[3].c_str());
			titem.Level = (unsigned char)atoi(sd[4].c_str());
			titem.bUseRandomness = (atoi(sd[7].c_str()) != 0);
			if ((titem.timerCmd == TCMD_ON) && (titem.Level == 0))
			{
				titem.Level = 100;
			}
			titem.Days = atoi(sd[5].c_str());
			titem.DeviceName = sd[6];
			if (AdjustScheduleItem(&titem, false) == true)
				m_scheduleitems.push_back(titem);
		}
	}

	//Add Setpoint Timers
	result = m_sql.safe_query(
		"SELECT T1.DeviceRowID, T1.Time, T1.Type, T1.Temperature, T1.Days, T2.Name,"
		" T1.[Date], T1.MDay, T1.Month, T1.Occurence, T1.ID"
		" FROM SetpointTimers as T1, DeviceStatus as T2"
		" WHERE ((T1.Active == 1) AND ((T1.TimerPlan == %d) OR (T1.TimerPlan == 9999)) AND (T2.ID == T1.DeviceRowID))"
		" ORDER BY T1.ID",
		m_sql.m_ActiveTimerPlan);
	if (!result.empty())
	{
		for (const auto& sd : result)
		{
			tScheduleItem titem;

			titem.bEnabled = true;
			titem.bIsScene = false;
			titem.bIsThermostat = true;
			titem.MDay = 0;
			titem.Month = 0;
			titem.Occurence = 0;
			titem.RowID = std::stoull(sd[0]);
			titem.TimerID = std::stoull(sd[10]);

			_eTimerType timerType = (_eTimerType)atoi(sd[2].c_str());

			if (timerType == TTYPE_FIXEDDATETIME)
			{
				std::string sdate = sd[6];
				if (sdate.size() != 10)
					continue; //invalid
				titem.startYear = (unsigned short)atoi(sdate.substr(0, 4).c_str());
				titem.startMonth = (unsigned char)atoi(sdate.substr(5, 2).c_str());
				titem.startDay = (unsigned char)atoi(sdate.substr(8, 2).c_str());
			}
			else if (timerType == TTYPE_MONTHLY)
			{
				std::string smday = sd[7];
				if (smday == "0")
					continue; //invalid
				titem.MDay = atoi(smday.c_str());
			}
			else if (timerType == TTYPE_MONTHLY_WD)
			{
				std::string socc = sd[9];
				if (socc == "0")
					continue; //invalid
				titem.Occurence = atoi(socc.c_str());
			}
			else if (timerType == TTYPE_YEARLY)
			{
				std::string smday = sd[7];
				std::string smonth = sd[8];
				if ((smday == "0") || (smonth == "0"))
					continue; //invalid
				titem.MDay = atoi(smday.c_str());
				titem.Month = atoi(smonth.c_str());
			}
			else if (timerType == TTYPE_YEARLY_WD)
			{
				std::string smonth = sd[8];
				std::string socc = sd[9];
				if ((smonth == "0") || (socc == "0"))
					continue; //invalid
				titem.Month = atoi(smonth.c_str());
				titem.Occurence = atoi(socc.c_str());
			}

			titem.startHour = (unsigned char)atoi(sd[1].substr(0, 2).c_str());
			titem.startMin = (unsigned char)atoi(sd[1].substr(3, 2).c_str());
			titem.startTime = 0;
			titem.timerType = timerType;
			titem.timerCmd = TCMD_ON;
			titem.Temperature = static_cast<float>(atof(sd[3].c_str()));
			titem.Level = 100;
			titem.bUseRandomness = false;
			titem.Days = atoi(sd[4].c_str());
			titem.DeviceName = sd[5];
			if (AdjustScheduleItem(&titem, false) == true)
				m_scheduleitems.push_back(titem);
		}
	}
}

void CScheduler::SetSunRiseSetTimes(const std::string& sSunRise, const std::string& sSunSet, const std::string& sSunAtSouth, const std::string& sCivTwStart, const std::string& sCivTwEnd, const std::string& sNautTwStart, const std::string& sNautTwEnd, const std::string& sAstTwStart, const std::string& sAstTwEnd)
{
	bool bReloadSchedules = false;
	bool bReloadSunRiseSet = false;

	{	//needed private scope for the lock
		std::lock_guard<std::mutex> l(m_mutex);

		time_t temptime;
		time_t atime = mytime(nullptr);
		struct tm ltime;
		localtime_r(&atime, &ltime);
		struct tm tm1;

		auto allSchedules = std::array<std::string, 9>{ sSunRise, sSunSet, sSunAtSouth, sCivTwStart, sCivTwEnd, sNautTwStart, sNautTwEnd, sAstTwStart, sAstTwEnd };
		time_t* allTimes[] = { &m_tSunRise, &m_tSunSet, &m_tSunAtSouth, &m_tCivTwStart, &m_tCivTwEnd, &m_tNautTwStart, &m_tNautTwEnd, &m_tAstTwStart, &m_tAstTwEnd };
		for (size_t a = 0; a < allSchedules.size(); a = a + 1)
		{
			//std::cout << allSchedules[a].c_str() << ' ';
			int hour = atoi(allSchedules[a].substr(0, 2).c_str());
			int min = atoi(allSchedules[a].substr(3, 2).c_str());
			int sec = atoi(allSchedules[a].substr(6, 2).c_str());

			constructTime(temptime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday, hour, min, sec, ltime.tm_isdst);
			if ((*allTimes[a] != temptime) && (temptime != 0))
			{
				if (*allTimes[a] == 0)
					bReloadSchedules = true;
				*allTimes[a] = temptime;
				bReloadSunRiseSet = true;
			}
		}
	}

	if (bReloadSchedules)
		ReloadSchedules();
	else if (bReloadSunRiseSet)
		AdjustSunRiseSetSchedules();
}

void CScheduler::AdjustSunRiseSetSchedules()
{
	for (auto &itt : m_scheduleitems)
	{
		if ((itt.timerType == TTYPE_BEFORESUNRISE) ||
			(itt.timerType == TTYPE_AFTERSUNRISE) ||
			(itt.timerType == TTYPE_BEFORESUNSET) ||
			(itt.timerType == TTYPE_AFTERSUNSET) ||

			(itt.timerType == TTYPE_BEFORESUNATSOUTH) ||
			(itt.timerType == TTYPE_AFTERSUNATSOUTH) ||
			(itt.timerType == TTYPE_BEFORECIVTWSTART) ||
			(itt.timerType == TTYPE_AFTERCIVTWSTART) ||
			(itt.timerType == TTYPE_BEFORECIVTWEND) ||
			(itt.timerType == TTYPE_AFTERCIVTWEND) ||
			(itt.timerType == TTYPE_BEFORENAUTTWSTART) ||
			(itt.timerType == TTYPE_AFTERNAUTTWSTART) ||
			(itt.timerType == TTYPE_BEFORENAUTTWEND) ||
			(itt.timerType == TTYPE_AFTERNAUTTWEND) ||
			(itt.timerType == TTYPE_BEFOREASTTWSTART) ||
			(itt.timerType == TTYPE_AFTERASTTWSTART) ||
			(itt.timerType == TTYPE_BEFOREASTTWEND) ||
			(itt.timerType == TTYPE_AFTERASTTWEND)
			)
		{
			AdjustScheduleItem(&itt, false);
		}
	}
}

bool CScheduler::AdjustScheduleItem(tScheduleItem* pItem, bool bForceAddDay)
{
	time_t atime = mytime(nullptr);
	time_t rtime = atime;
	struct tm ltime;
	localtime_r(&atime, &ltime);
	int isdst = ltime.tm_isdst;
	struct tm tm1;
	memset(&tm1, 0, sizeof(tm));
	tm1.tm_isdst = -1;

	if (bForceAddDay)
		ltime.tm_mday++;

	unsigned long HourMinuteOffset = (pItem->startHour * 3600) + (pItem->startMin * 60);

	int nRandomTimerFrame = 15;
	m_sql.GetPreferencesVar("RandomTimerFrame", nRandomTimerFrame);
	if (nRandomTimerFrame == 0)
		nRandomTimerFrame = 15;
	int roffset = 0;
	if (pItem->bUseRandomness)
	{
		if ((pItem->timerType == TTYPE_BEFORESUNRISE) ||
			(pItem->timerType == TTYPE_AFTERSUNRISE) ||
			(pItem->timerType == TTYPE_BEFORESUNSET) ||
			(pItem->timerType == TTYPE_AFTERSUNSET) ||

			(pItem->timerType == TTYPE_BEFORESUNATSOUTH) ||
			(pItem->timerType == TTYPE_AFTERSUNATSOUTH) ||
			(pItem->timerType == TTYPE_BEFORECIVTWSTART) ||
			(pItem->timerType == TTYPE_AFTERCIVTWSTART) ||
			(pItem->timerType == TTYPE_BEFORECIVTWEND) ||
			(pItem->timerType == TTYPE_AFTERCIVTWEND) ||
			(pItem->timerType == TTYPE_BEFORENAUTTWSTART) ||
			(pItem->timerType == TTYPE_AFTERNAUTTWSTART) ||
			(pItem->timerType == TTYPE_BEFORENAUTTWEND) ||
			(pItem->timerType == TTYPE_AFTERNAUTTWEND) ||
			(pItem->timerType == TTYPE_BEFOREASTTWSTART) ||
			(pItem->timerType == TTYPE_AFTERASTTWSTART) ||
			(pItem->timerType == TTYPE_BEFOREASTTWEND) ||
			(pItem->timerType == TTYPE_AFTERASTTWEND))
			roffset = rand() % (nRandomTimerFrame);
		else
			roffset = rand() % (nRandomTimerFrame * 2) - nRandomTimerFrame;
	}
	if ((pItem->timerType == TTYPE_ONTIME) ||
		(pItem->timerType == TTYPE_DAYSODD) ||
		(pItem->timerType == TTYPE_DAYSEVEN) ||
		(pItem->timerType == TTYPE_WEEKSODD) ||
		(pItem->timerType == TTYPE_WEEKSEVEN))
	{
		constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday, pItem->startHour, pItem->startMin, roffset * 60, isdst);
		while (rtime < atime + 60)
		{
			ltime.tm_mday++;
			constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, ltime.tm_mday, pItem->startHour, pItem->startMin, roffset * 60, isdst);
		}
		pItem->startTime = rtime;
		return true;
	}
	if (pItem->timerType == TTYPE_FIXEDDATETIME)
	{
		constructTime(rtime, tm1, pItem->startYear, pItem->startMonth, pItem->startDay, pItem->startHour, pItem->startMin, roffset * 60, isdst);
		if (rtime < atime)
			return false; //past date/time
		pItem->startTime = rtime;
		return true;
	}
	if (pItem->timerType == TTYPE_BEFORESUNSET)
	{
		if (m_tSunSet == 0)
			return false;
		rtime = m_tSunSet - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERSUNSET)
	{
		if (m_tSunSet == 0)
			return false;
		rtime = m_tSunSet + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORESUNRISE)
	{
		if (m_tSunRise == 0)
			return false;
		rtime = m_tSunRise - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERSUNRISE)
	{
		if (m_tSunRise == 0)
			return false;
		rtime = m_tSunRise + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORESUNATSOUTH)
	{
		if (m_tSunAtSouth == 0)
			return false;
		rtime = m_tSunAtSouth - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERSUNATSOUTH)
	{
		if (m_tSunAtSouth == 0)
			return false;
		rtime = m_tSunAtSouth + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORECIVTWSTART)
	{
		if (m_tCivTwStart == 0)
			return false;
		rtime = m_tCivTwStart - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERCIVTWSTART)
	{
		if (m_tCivTwStart == 0)
			return false;
		rtime = m_tCivTwStart + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORECIVTWEND)
	{
		if (m_tCivTwEnd == 0)
			return false;
		rtime = m_tCivTwEnd - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERCIVTWEND)
	{
		if (m_tCivTwEnd == 0)
			return false;
		rtime = m_tCivTwEnd + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORENAUTTWSTART)
	{
		if (m_tNautTwStart == 0)
			return false;
		rtime = m_tNautTwStart - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERNAUTTWSTART)
	{
		if (m_tNautTwStart == 0)
			return false;
		rtime = m_tNautTwStart + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFORENAUTTWEND)
	{
		if (m_tNautTwEnd == 0)
			return false;
		rtime = m_tNautTwEnd - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERNAUTTWEND)
	{
		if (m_tNautTwEnd == 0)
			return false;
		rtime = m_tNautTwEnd + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFOREASTTWSTART)
	{
		if (m_tAstTwStart == 0)
			return false;
		rtime = m_tAstTwStart - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERASTTWSTART)
	{
		if (m_tAstTwStart == 0)
			return false;
		rtime = m_tAstTwStart + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_BEFOREASTTWEND)
	{
		if (m_tAstTwEnd == 0)
			return false;
		rtime = m_tAstTwEnd - HourMinuteOffset - (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_AFTERASTTWEND)
	{
		if (m_tAstTwEnd == 0)
			return false;
		rtime = m_tAstTwEnd + HourMinuteOffset + (roffset * 60);
	}
	else if (pItem->timerType == TTYPE_MONTHLY)
	{
		constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, pItem->MDay, pItem->startHour, pItem->startMin, 0, isdst);

		while ((rtime < atime) || (tm1.tm_mday != pItem->MDay)) // past date/time OR mday exceeds max days in month
		{
			ltime.tm_mon++;
			constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, pItem->MDay, pItem->startHour, pItem->startMin, 0, isdst);
		}

		rtime += roffset * 60; // add randomness
		pItem->startTime = rtime;
		return true;
	}
	else if (pItem->timerType == TTYPE_MONTHLY_WD)
	{
		//pItem->Days: mon=1 .. sat=32, sun=64
		//convert to : sun=0, mon=1 .. sat=6
		int daynum = (int)log2(pItem->Days) + 1;
		if (daynum == 7) daynum = 0;

		boost::gregorian::nth_day_of_the_week_in_month::week_num Occurence = static_cast<boost::gregorian::nth_day_of_the_week_in_month::week_num>(pItem->Occurence);
		boost::gregorian::greg_weekday::weekday_enum Day = static_cast<boost::gregorian::greg_weekday::weekday_enum>(daynum);
		boost::gregorian::months_of_year Month = static_cast<boost::gregorian::months_of_year>(ltime.tm_mon + 1);

		typedef boost::gregorian::nth_day_of_the_week_in_month nth_dow;
		nth_dow ndm(Occurence, Day, Month);
		boost::gregorian::date d = ndm.get_date(ltime.tm_year + 1900);

		constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon + 1, d.day(), pItem->startHour, pItem->startMin, 0, isdst);

		if (rtime < atime) //past date/time
		{
			//schedule for next month
			ltime.tm_mon++;
			if (ltime.tm_mon == 12) // fix for roll over to next year
			{
				ltime.tm_mon = 0;
				ltime.tm_year++;
			}
			Month = static_cast<boost::gregorian::months_of_year>(ltime.tm_mon + 1);
			nth_dow ndm(Occurence, Day, Month);
			boost::gregorian::date d = ndm.get_date(ltime.tm_year + 1900);

			constructTime(rtime, tm1, ltime.tm_year + 1900, ltime.tm_mon, d.day(), pItem->startHour, pItem->startMin, 0, isdst);
		}

		rtime += roffset * 60; // add randomness
		pItem->startTime = rtime;
		return true;
	}
	else if (pItem->timerType == TTYPE_YEARLY)
	{
		constructTime(rtime, tm1, ltime.tm_year + 1900, pItem->Month, pItem->MDay, pItem->startHour, pItem->startMin, 0, isdst);

		while ((rtime < atime) || (tm1.tm_mday != pItem->MDay)) // past date/time OR mday exceeds max days in month
		{
			//schedule for next year
			ltime.tm_year++;
			constructTime(rtime, tm1, ltime.tm_year + 1900, pItem->Month, pItem->MDay, pItem->startHour, pItem->startMin, 0, isdst);
		}

		rtime += roffset * 60; // add randomness
		pItem->startTime = rtime;
		return true;
	}
	else if (pItem->timerType == TTYPE_YEARLY_WD)
	{
		//pItem->Days: mon=1 .. sat=32, sun=64
		//convert to : sun=0, mon=1 .. sat=6
		int daynum = (int)log2(pItem->Days) + 1;
		if (daynum == 7) daynum = 0;

		boost::gregorian::nth_day_of_the_week_in_month::week_num Occurence = static_cast<boost::gregorian::nth_day_of_the_week_in_month::week_num>(pItem->Occurence);
		boost::gregorian::greg_weekday::weekday_enum Day = static_cast<boost::gregorian::greg_weekday::weekday_enum>(daynum);
		boost::gregorian::months_of_year Month = static_cast<boost::gregorian::months_of_year>(ltime.tm_mon + 1);

		typedef boost::gregorian::nth_day_of_the_week_in_month nth_dow;
		nth_dow ndm(Occurence, Day, Month);
		boost::gregorian::date d = ndm.get_date(ltime.tm_year + 1900);

		constructTime(rtime, tm1, ltime.tm_year + 1900, pItem->Month, d.day(), pItem->startHour, pItem->startMin, 0, isdst);

		if (rtime < atime) //past date/time
		{
			//schedule for next year
			ltime.tm_year++;
			constructTime(rtime, tm1, ltime.tm_year + 1900, pItem->Month, d.day(), pItem->startHour, pItem->startMin, 0, isdst);
		}

		rtime += roffset * 60; // add randomness
		pItem->startTime = rtime;
		return true;
	}
	else
		return false; //unknown timer type

	if (tm1.tm_isdst == -1) // rtime was loaded from sunset/sunrise values; need to initialize tm1
	{
		if (bForceAddDay) // Adjust timer by 1 day if item is scheduled for next day
			rtime += 86400;

		//FIXME: because we are referencing the wrong date for sunset/sunrise values (i.e. today)
		//	 it may lead to incorrect results if we use localtime_r for finding the correct time
		while (rtime < atime + 60)
		{
			rtime += 86400;
		}
		pItem->startTime = rtime;
		return true;
		// end of FIXME block

		//FIXME: keep for future reference
		//tm1.tm_isdst = isdst; // load our current DST value and allow localtime_r to correct our 'mistake'
		//localtime_r(&rtime, &tm1);
		//isdst = tm1.tm_isdst;
	}

	// Adjust timer by 1 day if we are in the past
	//FIXME: this part of the code currently seems impossible to reach, but I may be overseeing something. Therefore:
	//	 it should be noted that constructTime() can return a time where tm1.tm_hour is different from pItem->startHour
	//	 The main cause for this to happen is that startHour is not a valid time on that day due to changing to Summertime
	//	 in which case the hour will be incremented by 1. By using tm1.tm_hour here this adjustment will propagate into
	//	 whatever following day may result from this loop and cause the timer to be set 1 hour later than it should be.
	while (rtime < atime + 60)
	{
		tm1.tm_mday++;
		struct tm tm2;
		constructTime(rtime, tm2, tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday, tm1.tm_hour, tm1.tm_min, tm1.tm_sec, isdst);
	}

	pItem->startTime = rtime;
	return true;
}

void CScheduler::Do_Work()
{
	while (!IsStopRequested(1000))
	{
		time_t atime = mytime(nullptr);
		struct tm ltime;
		localtime_r(&atime, &ltime);


		if (ltime.tm_sec % 12 == 0) {
			m_mainworker.HeartbeatUpdate("Scheduler");
		}

		CheckSchedules();

		if (ltime.tm_sec == 0) {
			DeleteExpiredTimers();
		}
	}
	_log.Log(LOG_STATUS, "Scheduler stopped...");
}

void CScheduler::CheckSchedules()
{
	std::lock_guard<std::mutex> l(m_mutex);

	time_t atime = mytime(nullptr);
	struct tm ltime;
	localtime_r(&atime, &ltime);

	for (auto& item : m_scheduleitems)
	{
		if ((item.bEnabled) && (atime > item.startTime))
		{
			//check if we are on a valid day
			bool bOkToFire = false;
			if (item.timerType == TTYPE_FIXEDDATETIME)
			{
				bOkToFire = true;
			}
			else if (item.timerType == TTYPE_DAYSODD)
			{
				bOkToFire = (ltime.tm_mday % 2 != 0);
			}
			else if (item.timerType == TTYPE_DAYSEVEN)
			{
				bOkToFire = (ltime.tm_mday % 2 == 0);
			}
			else
			{
				if (item.Days & 0x80)
				{
					//everyday
					bOkToFire = true;
				}
				else if (item.Days & 0x100)
				{
					//weekdays
					if ((ltime.tm_wday > 0) && (ltime.tm_wday < 6))
						bOkToFire = true;
				}
				else if (item.Days & 0x200)
				{
					//weekends
					if ((ltime.tm_wday == 0) || (ltime.tm_wday == 6))
						bOkToFire = true;
				}
				else
				{
					//custom days
					if ((item.Days & 0x01) && (ltime.tm_wday == 1))
						bOkToFire = true;//Monday
					if ((item.Days & 0x02) && (ltime.tm_wday == 2))
						bOkToFire = true;//Tuesday
					if ((item.Days & 0x04) && (ltime.tm_wday == 3))
						bOkToFire = true;//Wednesday
					if ((item.Days & 0x08) && (ltime.tm_wday == 4))
						bOkToFire = true;//Thursday
					if ((item.Days & 0x10) && (ltime.tm_wday == 5))
						bOkToFire = true;//Friday
					if ((item.Days & 0x20) && (ltime.tm_wday == 6))
						bOkToFire = true;//Saturday
					if ((item.Days & 0x40) && (ltime.tm_wday == 0))
						bOkToFire = true;//Sunday
				}
				if (bOkToFire)
				{
					if ((item.timerType == TTYPE_WEEKSODD) || (item.timerType == TTYPE_WEEKSEVEN))
					{
						struct tm timeinfo;
						localtime_r(&item.startTime, &timeinfo);

						boost::gregorian::date d = boost::gregorian::date(
							timeinfo.tm_year + 1900,
							timeinfo.tm_mon + 1,
							timeinfo.tm_mday);
						int w = d.week_number();

						if (item.timerType == TTYPE_WEEKSODD)
							bOkToFire = (w % 2 != 0);
						else
							bOkToFire = (w % 2 == 0);
					}
				}
			}
			if (bOkToFire)
			{
				char ltimeBuf[30];
				strftime(ltimeBuf, sizeof(ltimeBuf), "%Y-%m-%d %H:%M:%S", &ltime);

				if (item.bIsScene == true)
					_log.Log(LOG_STATUS, "Schedule item started! Name: %s, Type: %s, SceneID: %" PRIu64 ", Time: %s",
						item.DeviceName.c_str(), Timer_Type_Desc(item.timerType), item.RowID, ltimeBuf);
				else if (item.bIsThermostat == true)
					_log.Log(LOG_STATUS,
						"Schedule item started! Name: %s, Type: %s, ThermostatID: %" PRIu64 ", Time: %s",
						item.DeviceName.c_str(), Timer_Type_Desc(item.timerType), item.RowID, ltimeBuf);
				else
					_log.Log(LOG_STATUS, "Schedule item started! Name: %s, Type: %s, DevID: %" PRIu64 ", Time: %s",
						item.DeviceName.c_str(), Timer_Type_Desc(item.timerType), item.RowID, ltimeBuf);
				std::string switchcmd;
				if (item.timerCmd == TCMD_ON)
					switchcmd = "On";
				else if (item.timerCmd == TCMD_OFF)
					switchcmd = "Off";
				if (switchcmd.empty())
				{
					_log.Log(LOG_ERROR, "Unknown switch command in timer!!....");
				}
				else
				{
					if (item.bIsScene == true)
					{
						if (!m_mainworker.SwitchScene(item.RowID, switchcmd, "timer"))
						{
							_log.Log(LOG_ERROR, "Error switching Scene command, SceneID: %" PRIu64 ", Time: %s",
								item.RowID, ltimeBuf);
						}
					}
					else if (item.bIsThermostat == true)
					{
						std::stringstream sstr;
						sstr << item.RowID;
						if (!m_mainworker.SetSetPoint(sstr.str(), item.Temperature))
						{
							_log.Log(LOG_ERROR,
								"Error setting thermostat setpoint, ThermostatID: %" PRIu64 ", Time: %s",
								item.RowID, ltimeBuf);
						}
					}
					else
					{
						//Get SwitchType
						std::vector<std::vector<std::string> > result;
						result = m_sql.safe_query(
							"SELECT Type,SubType,SwitchType FROM DeviceStatus WHERE (ID == %" PRIu64 ")",
							item.RowID);
						if (!result.empty())
						{
							std::vector<std::string> sd = result[0];

							unsigned char dType = atoi(sd[0].c_str());
							unsigned char dSubType = atoi(sd[1].c_str());
							_eSwitchType switchtype = (_eSwitchType)atoi(sd[2].c_str());
							std::string lstatus;
							int llevel = 0;
							bool bHaveDimmer = false;
							bool bHaveGroupCmd = false;
							int maxDimLevel = 0;

							GetLightStatus(dType, dSubType, switchtype, 0, "", lstatus, llevel, bHaveDimmer, maxDimLevel, bHaveGroupCmd);
							int ilevel = maxDimLevel;
							if (switchtype == STYPE_Blinds)
							{
								if (item.timerCmd == TCMD_ON)
									switchcmd = "Open";
								else if (item.timerCmd == TCMD_OFF)
									switchcmd = "Close";
							}
							else if (
								(switchtype == STYPE_BlindsPercentage)
								|| (switchtype == STYPE_BlindsPercentageWithStop)
								)
							{
								if ((item.Level > 0) && (item.Level < 100))
								{
									// set position to value between 1 and 99 %
									switchcmd = "Set Level";
									float fLevel = (maxDimLevel / 100.0F) * item.Level;
									ilevel = round(fLevel);
									if (ilevel > maxDimLevel)
										ilevel = maxDimLevel;
								}
								else if (item.timerCmd == TCMD_ON) // no percentage set (0 or 100)
								{
									switchcmd = "Open";
									ilevel = 100;
								}
								else if (item.timerCmd == TCMD_OFF) // no percentage set (0 or 100)
								{
									switchcmd = "Close";
									ilevel = 0;
								}
							}
							else if ((switchtype == STYPE_Dimmer) && (maxDimLevel != 0))
							{
								if (item.timerCmd == TCMD_ON)
								{
									switchcmd = "Set Level";
									float fLevel = (maxDimLevel / 100.0F) * item.Level;
									ilevel = round(fLevel);
									if (ilevel > maxDimLevel)
										ilevel = maxDimLevel;
								}
							}
							else if (switchtype == STYPE_Selector) {
								if (item.timerCmd == TCMD_ON)
								{
									switchcmd = "Set Level";
									ilevel = item.Level;
								}
								else if (item.timerCmd == TCMD_OFF)
								{
									ilevel = 0; // force level to a valid value for Selector
								}
							}
							if (m_mainworker.SwitchLight(item.RowID, switchcmd, ilevel, item.Color, false, 0, "timer") == MainWorker::SL_ERROR)
							{
								_log.Log(LOG_ERROR,
									"Error sending switch command, DevID: %" PRIu64 ", Time: %s",
									item.RowID, ltimeBuf);
							}
						}
					}
				}
			}
			if (!AdjustScheduleItem(&item, true))
			{
				//something is wrong, probably no sunset/rise
				if (item.timerType != TTYPE_FIXEDDATETIME)
				{
					item.startTime += atime + (24 * 3600);
				}
				else
				{
					//Disable timer
					item.bEnabled = false;
				}
			}
		}
	}
}

void CScheduler::DeleteExpiredTimers()
{
	char szDate[40];
	char szTime[40];
	time_t now = mytime(nullptr);
	struct tm tmnow;
	localtime_r(&now, &tmnow);
	sprintf(szDate, "%04d-%02d-%02d", tmnow.tm_year + 1900, tmnow.tm_mon + 1, tmnow.tm_mday);
	sprintf(szTime, "%02d:%02d", tmnow.tm_hour, tmnow.tm_min);
	int iExpiredTimers = 0;

	std::vector<std::vector<std::string> > result;
	// Check Timers
	result = m_sql.safe_query("SELECT ID FROM Timers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
		TTYPE_FIXEDDATETIME,
		szDate,
		szDate,
		szTime
	);
	if (!result.empty()) {
		m_sql.safe_query("DELETE FROM Timers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
			TTYPE_FIXEDDATETIME,
			szDate,
			szDate,
			szTime
		);
		iExpiredTimers += static_cast<int>(result.size());
	}

	// Check SceneTimers
	result = m_sql.safe_query("SELECT ID FROM SceneTimers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
		TTYPE_FIXEDDATETIME,
		szDate,
		szDate,
		szTime
	);
	if (!result.empty()) {
		m_sql.safe_query("DELETE FROM SceneTimers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
			TTYPE_FIXEDDATETIME,
			szDate,
			szDate,
			szTime
		);
		iExpiredTimers += static_cast<int>(result.size());
	}

	// Setpoint timers
	result = m_sql.safe_query("SELECT ID FROM SetpointTimers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
		TTYPE_FIXEDDATETIME,
		szDate,
		szDate,
		szTime
	);
	if (!result.empty()) {
		m_sql.safe_query("DELETE FROM SetpointTimers WHERE (Type == %i AND ((Date < '%q') OR (Date == '%q' AND Time < '%q')))",
			TTYPE_FIXEDDATETIME,
			szDate,
			szDate,
			szTime
		);
		iExpiredTimers += static_cast<int>(result.size());
	}
	

	if (iExpiredTimers > 0) {
		_log.Log(LOG_STATUS, "Purged %i expired (scene)timer(s)", iExpiredTimers);

		ReloadSchedules();
	}
}

//Webserver helpers
namespace http {
	namespace server {
		void CWebServer::Cmd_GetSchedules(WebEmSession& session, const request& req, Json::Value& root)
		{
			std::string rfilter = request::findValue(&req, "filter");

			root["status"] = "OK";
			root["title"] = "getschedules";

			std::vector<std::vector<std::string> > tot_result;
			std::vector<std::vector<std::string> > tmp_result;

			//Retrieve active schedules
			std::vector<tScheduleItem> schedules = m_mainworker.m_scheduler.GetScheduleItems();

			//Add Timers
			if ((rfilter == "all") || (rfilter.empty()) || (rfilter == "device"))
			{
				tmp_result = m_sql.safe_query(
					"SELECT t.ID, t.Active, d.[Name], t.DeviceRowID AS ID, 0 AS IsScene, 0 AS IsThermostat,"
					" t.[Date], t.Time, t.Type, t.Cmd, t.Level, t.Color, 0 AS Temperature, t.Days,"
					" t.UseRandomness, t.MDay, t.Month, t.Occurence"
					" FROM Timers AS t, DeviceStatus AS d"
					" WHERE (d.ID == t.DeviceRowID) AND (t.TimerPlan==%d)"
					" ORDER BY d.[Name], t.Time",
					m_sql.m_ActiveTimerPlan);
				if (!tmp_result.empty())
					tot_result.insert(tot_result.end(), tmp_result.begin(), tmp_result.end());
			}

			//Add Scene-Timers
			if ((rfilter == "all") || (rfilter.empty()) || (rfilter == "scene"))
			{
				tmp_result = m_sql.safe_query(
					"SELECT t.ID, t.Active, s.[Name], t.SceneRowID AS ID, 1 AS IsScene, 0 AS IsThermostat"
					", t.[Date], t.Time, t.Type, t.Cmd, t.Level, '' AS Color, 0 AS Temperature, t.Days,"
					" t.UseRandomness, t.MDay, t.Month, t.Occurence"
					" FROM SceneTimers AS t, Scenes AS s"
					" WHERE (s.ID == t.SceneRowID) AND (t.TimerPlan==%d)"
					" ORDER BY s.[Name], t.Time",
					m_sql.m_ActiveTimerPlan);
				if (!tmp_result.empty())
					tot_result.insert(tot_result.end(), tmp_result.begin(), tmp_result.end());
			}

			//Add Setpoint-Timers
			if ((rfilter == "all") || (rfilter.empty()) || (rfilter == "thermostat"))
			{
				tmp_result = m_sql.safe_query(
					"SELECT t.ID, t.Active, d.[Name], t.DeviceRowID AS ID, 0 AS IsScene, 1 AS IsThermostat,"
					" t.[Date], t.Time, t.Type, 0 AS Cmd, 0 AS Level, '' AS Color, t.Temperature, t.Days,"
					" 0 AS UseRandomness, t.MDay, t.Month, t.Occurence"
					" FROM SetpointTimers AS t, DeviceStatus AS d"
					" WHERE (d.ID == t.DeviceRowID) AND (t.TimerPlan==%d)"
					" ORDER BY d.[Name], t.Time",
					m_sql.m_ActiveTimerPlan);
				if (!tmp_result.empty())
					tot_result.insert(tot_result.end(), tmp_result.begin(), tmp_result.end());
			}

			if (!tot_result.empty())
			{
				int ii = 0;
				for (const auto& sd : tot_result)
				{
					int iTimerIdx = atoi(sd[0].c_str());
					bool bActive = atoi(sd[1].c_str()) != 0;
					bool bIsScene = atoi(sd[4].c_str()) != 0;
					bool bIsThermostat = atoi(sd[5].c_str()) != 0;
					int iDays = atoi(sd[13].c_str());
					char ltimeBuf[30] = "";

					if (bActive)
					{
						//Find Timer in active schedules and retrieve ScheduleDate
						tScheduleItem sitem;
						sitem.TimerID = iTimerIdx;
						sitem.bIsScene = bIsScene;
						sitem.bIsThermostat = bIsThermostat;
						auto it = std::find(schedules.begin(), schedules.end(), sitem);
						if (it != schedules.end()) {
							struct tm timeinfo;
							localtime_r(&it->startTime, &timeinfo);
							strftime(ltimeBuf, sizeof(ltimeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
						}
					}

					unsigned char iLevel = atoi(sd[10].c_str());
					if (iLevel == 0)
						iLevel = 100;

					int iTimerType = atoi(sd[8].c_str());
					std::string sdate = sd[6];
					if ((iTimerType == TTYPE_FIXEDDATETIME) && (sdate.size() == 10))
					{
						char szTmp[30];
						int Year = atoi(sdate.substr(0, 4).c_str());
						int Month = atoi(sdate.substr(5, 2).c_str());
						int Day = atoi(sdate.substr(8, 2).c_str());
						sprintf(szTmp, "%04d-%02d-%02d", Year, Month, Day);
						sdate = szTmp;
					}
					else
						sdate = "";

					root["result"][ii]["TimerID"] = iTimerIdx;
					root["result"][ii]["Active"] = bActive ? "true" : "false";
					root["result"][ii]["Type"] = bIsScene ? "Scene" : "Device";
					root["result"][ii]["IsThermostat"] = bIsThermostat ? "true" : "false";
					root["result"][ii]["DevName"] = sd[2];
					root["result"][ii]["DeviceRowID"] = atoi(sd[3].c_str()); //TODO: Isn't this a 64 bit device index?
					root["result"][ii]["Date"] = sdate;
					root["result"][ii]["Time"] = sd[7];
					root["result"][ii]["TimerType"] = iTimerType;
					root["result"][ii]["TimerTypeStr"] = Timer_Type_Desc(iTimerType);
					root["result"][ii]["Days"] = iDays;
					root["result"][ii]["MDay"] = atoi(sd[15].c_str());
					root["result"][ii]["Month"] = atoi(sd[16].c_str());
					root["result"][ii]["Occurence"] = atoi(sd[17].c_str());
					root["result"][ii]["ScheduleDate"] = ltimeBuf;
					if (bIsThermostat)
					{
						root["result"][ii]["Temperature"] = sd[12];
					}
					else
					{
						root["result"][ii]["TimerCmd"] = atoi(sd[9].c_str());
						root["result"][ii]["Level"] = iLevel;
						root["result"][ii]["Color"] = sd[11];
						root["result"][ii]["Randomness"] = (atoi(sd[14].c_str()) != 0) ? "true" : "false";
					}
					ii++;
				}
			}
		}
		void CWebServer::Cmd_GetTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			if (idx == 0)
				return;
			root["status"] = "OK";
			root["title"] = "gettimers";

			std::vector<std::vector<std::string> > result;

			result = m_sql.safe_query("SELECT SwitchType FROM DeviceStatus WHERE (ID==%" PRIu64 ")", idx);
			if (result.empty())
				return;

			_eSwitchType switchtype = (_eSwitchType)std::stoi(result[0][0]);
			const bool bIsBlinds = (
				switchtype == STYPE_Blinds
				|| switchtype == STYPE_BlindsPercentage
				|| switchtype == STYPE_BlindsPercentageWithStop
				|| switchtype == STYPE_VenetianBlindsEU
				|| switchtype == STYPE_VenetianBlindsUS
				);


			result = m_sql.safe_query("SELECT ID, Active, [Date], Time, Type, Cmd, Level, Color, Days, UseRandomness, MDay, Month, Occurence, TimerPlan FROM Timers WHERE (DeviceRowID==%" PRIu64 ") AND ((TimerPlan==%d) OR (TimerPlan==9999)) ORDER BY ID",
				idx, m_sql.m_ActiveTimerPlan);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					unsigned char iLevel = atoi(sd[6].c_str());
					if ((iLevel == 0) && (!bIsBlinds))
						iLevel = 100;

					int iTimerType = atoi(sd[4].c_str());
					std::string sdate = sd[2];
					if ((iTimerType == TTYPE_FIXEDDATETIME) && (sdate.size() == 10))
					{
						int Year = atoi(sdate.substr(0, 4).c_str());
						int Month = atoi(sdate.substr(5, 2).c_str());
						int Day = atoi(sdate.substr(8, 2).c_str());
						sdate = std_format("%04d-%02d-%02d", Year, Month, Day);
					}
					else
						sdate = "";

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Active"] = (atoi(sd[1].c_str()) == 0) ? "false" : "true";
					root["result"][ii]["Date"] = sdate;
					root["result"][ii]["Time"] = sd[3].substr(0, 5);
					root["result"][ii]["Type"] = iTimerType;
					root["result"][ii]["Cmd"] = atoi(sd[5].c_str());
					root["result"][ii]["Level"] = iLevel;
					root["result"][ii]["Color"] = sd[7];
					root["result"][ii]["Days"] = atoi(sd[8].c_str());
					root["result"][ii]["Randomness"] = (atoi(sd[9].c_str()) == 0) ? "false" : "true";
					root["result"][ii]["MDay"] = atoi(sd[10].c_str());
					root["result"][ii]["Month"] = atoi(sd[11].c_str());
					root["result"][ii]["Occurence"] = atoi(sd[12].c_str());
					root["result"][ii]["Persistent"] = (atoi(sd[13].c_str()) == 9999) ? "true" : "false";
					ii++;
				}
			}
		}

		void CWebServer::Cmd_SetActiveTimerPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			root["title"] = "SetActiveTimerPlan";
			int rnOldvalue = 0;
			int rnvalue = 0;
			std::string sValue = request::findValue(&req, "ActiveTimerPlan");
			m_sql.GetPreferencesVar("ActiveTimerPlan", rnOldvalue);
			rnvalue = (!sValue.empty() ? atoi(sValue.c_str()) : rnOldvalue);
			if (rnOldvalue != rnvalue)
			{
				std::vector<std::vector<std::string> > result;
				result = m_sql.safe_query("SELECT Name FROM TimerPlans WHERE (ID == %d)", rnvalue);
				if (result.empty())
				{
					_log.Debug(DEBUG_HARDWARE, "Scheduler Timerplan not found! (%d)", rnvalue);
					return; //timerplan not found!
				}
				_log.Log(LOG_STATUS, "Scheduler Timerplan changed (%d - %s)", rnvalue, result[0][0].c_str());
				m_sql.UpdatePreferencesVar("ActiveTimerPlan", rnvalue);
				m_sql.m_ActiveTimerPlan = rnvalue;
				m_mainworker.m_scheduler.ReloadSchedules();
				root["status"] = "OK";
			}
		}

		void CWebServer::Cmd_AddTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string randomness = request::findValue(&req, "randomness");
			std::string scmd = request::findValue(&req, "command");
			std::string sdays = request::findValue(&req, "days");
			std::string slevel = request::findValue(&req, "level");	//in percentage
			std::string scolor = request::findValue(&req, "color");
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");
			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (randomness.empty()) || (scmd.empty()) || (sdays.empty()))
				return;
			unsigned char iTimerType = atoi(stimertype.c_str());

			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			unsigned char icmd = atoi(scmd.c_str());
			int days = atoi(sdays.c_str());
			unsigned char level = atoi(slevel.c_str());
			_tColor color = _tColor(scolor);
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "AddTimer";
			m_sql.safe_query(
				"INSERT INTO Timers (Active, DeviceRowID, [Date], Time, Type, UseRandomness, Cmd, Level, Color, Days, MDay, Month, Occurence, TimerPlan) VALUES (%d,'%q','%04d-%02d-%02d','%02d:%02d',%d,%d,%d,%d,'%q',%d,%d,%d,%d,%d)",
				(active == "true") ? 1 : 0,
				idx.c_str(),
				Year, Month, Day,
				hour, min,
				iTimerType,
				(randomness == "true") ? 1 : 0,
				icmd,
				level,
				color.toJSONString().c_str(),
				days,
				mday,
				month,
				occurence,
				timer_plan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_UpdateTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string randomness = request::findValue(&req, "randomness");
			std::string scmd = request::findValue(&req, "command");
			std::string sdays = request::findValue(&req, "days");
			std::string slevel = request::findValue(&req, "level");	//in percentage
			std::string scolor = request::findValue(&req, "color");
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");
			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (randomness.empty()) || (scmd.empty()) || (sdays.empty()))
				return;

			unsigned char iTimerType = atoi(stimertype.c_str());

			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			unsigned char icmd = atoi(scmd.c_str());
			int days = atoi(sdays.c_str());
			unsigned char level = atoi(slevel.c_str());
			_tColor color = _tColor(scolor);
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "UpdateTimer";
			m_sql.safe_query(
				"UPDATE Timers SET Active=%d, [Date]='%04d-%02d-%02d', Time='%02d:%02d', Type=%d, UseRandomness=%d, Cmd=%d, Level=%d, Color='%q', Days=%d, MDay=%d, Month=%d, Occurence=%d, TimerPlan=%d WHERE (ID == '%q')",
				(active == "true") ? 1 : 0,
				Year, Month, Day,
				hour, min,
				iTimerType,
				(randomness == "true") ? 1 : 0,
				icmd,
				level,
				color.toJSONString().c_str(),
				days,
				mday,
				month,
				occurence,
				timer_plan,
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DeleteTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeleteTimer";
			m_sql.safe_query(
				"DELETE FROM Timers WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_EnableTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "EnableTimer";
			m_sql.safe_query(
				"UPDATE Timers SET Active=1 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DisableTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DisableTimer";
			m_sql.safe_query(
				"UPDATE Timers SET Active=0 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_ClearTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "ClearTimer";
			m_sql.safe_query(
				"DELETE FROM Timers WHERE (DeviceRowID == '%q') AND ((TimerPlan == %d) OR (TimerPlan == 9999))",
				idx.c_str(),
				m_sql.m_ActiveTimerPlan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_GetSetpointTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			if (idx == 0)
				return;
			root["status"] = "OK";
			root["title"] = "getsetpointtimers";
			char szTmp[50];

			std::vector<std::vector<std::string> > result;
			result = m_sql.safe_query("SELECT ID, Active, [Date], Time, Type, Temperature, Days, MDay, Month, Occurence, TimerPlan FROM SetpointTimers WHERE (DeviceRowID=%" PRIu64 ") AND ((TimerPlan==%d) OR (TimerPlan==9999)) ORDER BY ID",
				idx, m_sql.m_ActiveTimerPlan);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					int iTimerType = atoi(sd[4].c_str());
					std::string sdate = sd[2];
					if ((iTimerType == TTYPE_FIXEDDATETIME) && (sdate.size() == 10))
					{
						int Year = atoi(sdate.substr(0, 4).c_str());
						int Month = atoi(sdate.substr(5, 2).c_str());
						int Day = atoi(sdate.substr(8, 2).c_str());
						sprintf(szTmp, "%04d-%02d-%02d", Year, Month, Day);
						sdate = szTmp;
					}
					else
						sdate = "";

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Active"] = (atoi(sd[1].c_str()) == 0) ? "false" : "true";
					root["result"][ii]["Date"] = sdate;
					root["result"][ii]["Time"] = sd[3].substr(0, 5);
					root["result"][ii]["Type"] = iTimerType;
					root["result"][ii]["Temperature"] = atof(sd[5].c_str());
					root["result"][ii]["Days"] = atoi(sd[6].c_str());
					root["result"][ii]["MDay"] = atoi(sd[7].c_str());
					root["result"][ii]["Month"] = atoi(sd[8].c_str());
					root["result"][ii]["Occurence"] = atoi(sd[9].c_str());
					root["result"][ii]["Persistent"] = (atoi(sd[10].c_str()) == 9999) ? "true" : "false";
					ii++;
				}
			}
		}
		void CWebServer::Cmd_AddSetpointTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string stvalue = request::findValue(&req, "tvalue");
			std::string sdays = request::findValue(&req, "days");
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");

			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (stvalue.empty()) || (sdays.empty()))
				return;

			unsigned char iTimerType = atoi(stimertype.c_str());
			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			int days = atoi(sdays.c_str());
			float temperature = static_cast<float>(atof(stvalue.c_str()));
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "AddSetpointTimer";

			std::string szDate = std_format("%04d-%02d-%02d", Year, Month, Day);
			std::string szTime = std_format("%02d:%02d", hour, min);

			//Check for duplicate
			auto result = m_sql.safe_query("SELECT ID FROM SetpointTimers WHERE (DeviceRowID==%s) AND ([Date]=='%q') AND (Time=='%q') AND (Type==%d) AND (TimerPlan==%d)",
				idx.c_str(),
				szDate.c_str(),
				szTime.c_str(),
				iTimerType,
				timer_plan
			);
			if (!result.empty())
				return; //duplicate!

			m_sql.safe_query(
				"INSERT INTO SetpointTimers (Active, DeviceRowID, [Date], Time, Type, Temperature, Days, MDay, Month, Occurence, TimerPlan) VALUES (%d,'%q','%q','%q',%d,%.1f,%d,%d,%d,%d,%d)",
				(active == "true") ? 1 : 0,
				idx.c_str(),
				szDate.c_str(),
				szTime.c_str(),
				iTimerType,
				temperature,
				days,
				mday,
				month,
				occurence,
				timer_plan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_UpdateSetpointTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string stvalue = request::findValue(&req, "tvalue");
			std::string sdays = request::findValue(&req, "days");
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");
			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (stvalue.empty()) || (sdays.empty()))
				return;

			unsigned char iTimerType = atoi(stimertype.c_str());
			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			int days = atoi(sdays.c_str());
			float tempvalue = static_cast<float>(atof(stvalue.c_str()));
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "UpdateSetpointTimer";
			m_sql.safe_query(
				"UPDATE SetpointTimers SET Active=%d, [Date]='%04d-%02d-%02d', Time='%02d:%02d', Type=%d, Temperature=%.1f, Days=%d, MDay=%d, Month=%d, Occurence=%d, TimerPlan=%d WHERE (ID == '%q')",
				(active == "true") ? 1 : 0,
				Year, Month, Day,
				hour, min,
				iTimerType,
				tempvalue,
				days,
				mday,
				month,
				occurence,
				timer_plan,
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DeleteSetpointTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeleteSetpointTimer";
			m_sql.safe_query(
				"DELETE FROM SetpointTimers WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_EnableSetpointTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "EnableSetpointTimer";
			m_sql.safe_query(
				"UPDATE SetpointTimers SET Active=1 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DisableSetpointTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DisableSetpointTimer";
			m_sql.safe_query(
				"UPDATE SetpointTimers SET Active=0 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_ClearSetpointTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;

			int timer_plan = m_sql.m_ActiveTimerPlan;

			root["status"] = "OK";
			root["title"] = "ClearSetpointTimers";
			m_sql.safe_query(
				"DELETE FROM SetpointTimers WHERE (DeviceRowID == '%q') AND ((TimerPlan==%d) OR (TimerPlan==9999))",
				idx.c_str(),
				timer_plan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_GetSceneTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			uint64_t idx = 0;
			if (!request::findValue(&req, "idx").empty())
			{
				idx = std::stoull(request::findValue(&req, "idx"));
			}
			if (idx == 0)
				return;
			root["status"] = "OK";
			root["title"] = "getscenetimers";

			char szTmp[40];

			std::vector<std::vector<std::string> > result;
			result = m_sql.safe_query("SELECT ID, Active, [Date], Time, Type, Cmd, Level, Days, UseRandomness, MDay, Month, Occurence, TimerPlan FROM SceneTimers WHERE (SceneRowID==%" PRIu64 ") AND ((TimerPlan==%d) OR (TimerPlan==9999)) ORDER BY ID",
				idx, m_sql.m_ActiveTimerPlan);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					unsigned char iLevel = atoi(sd[6].c_str());
					if (iLevel == 0)
						iLevel = 100;

					int iTimerType = atoi(sd[4].c_str());
					std::string sdate = sd[2];
					if ((iTimerType == TTYPE_FIXEDDATETIME) && (sdate.size() == 10))
					{
						int Year = atoi(sdate.substr(0, 4).c_str());
						int Month = atoi(sdate.substr(5, 2).c_str());
						int Day = atoi(sdate.substr(8, 2).c_str());
						sprintf(szTmp, "%04d-%02d-%02d", Year, Month, Day);
						sdate = szTmp;
					}
					else
						sdate = "";

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Active"] = (atoi(sd[1].c_str()) == 0) ? "false" : "true";
					root["result"][ii]["Date"] = sdate;
					root["result"][ii]["Time"] = sd[3].substr(0, 5);
					root["result"][ii]["Type"] = iTimerType;
					root["result"][ii]["Cmd"] = atoi(sd[5].c_str());
					root["result"][ii]["Level"] = iLevel;
					root["result"][ii]["Days"] = atoi(sd[7].c_str());
					root["result"][ii]["Randomness"] = (atoi(sd[8].c_str()) == 0) ? "false" : "true";
					root["result"][ii]["MDay"] = atoi(sd[9].c_str());
					root["result"][ii]["Month"] = atoi(sd[10].c_str());
					root["result"][ii]["Occurence"] = atoi(sd[11].c_str());
					root["result"][ii]["Persistent"] = (atoi(sd[12].c_str()) == 9999) ? "true" : "false";
					ii++;
				}
			}
		}

		void CWebServer::Cmd_AddSceneTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string randomness = request::findValue(&req, "randomness");
			std::string scmd = request::findValue(&req, "command");
			std::string sdays = request::findValue(&req, "days");
			std::string slevel = request::findValue(&req, "level");	//in percentage
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");

			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (randomness.empty()) || (scmd.empty()) || (sdays.empty()))
				return;

			unsigned char iTimerType = atoi(stimertype.c_str());
			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			unsigned char icmd = atoi(scmd.c_str());
			int days = atoi(sdays.c_str());
			unsigned char level = atoi(slevel.c_str());
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "AddSceneTimer";
			m_sql.safe_query(
				"INSERT INTO SceneTimers (Active, SceneRowID, [Date], Time, Type, UseRandomness, Cmd, Level, Days, MDay, Month, Occurence, TimerPlan) VALUES (%d,'%q','%04d-%02d-%02d','%02d:%02d',%d,%d,%d,%d,%d,%d,%d,%d,%d)",
				(active == "true") ? 1 : 0,
				idx.c_str(),
				Year, Month, Day,
				hour, min,
				iTimerType,
				(randomness == "true") ? 1 : 0,
				icmd,
				level,
				days,
				mday,
				month,
				occurence,
				timer_plan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_UpdateSceneTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			std::string active = request::findValue(&req, "active");
			std::string stimertype = request::findValue(&req, "timertype");
			std::string sdate = request::findValue(&req, "date");
			std::string shour = request::findValue(&req, "hour");
			std::string smin = request::findValue(&req, "min");
			std::string randomness = request::findValue(&req, "randomness");
			std::string scmd = request::findValue(&req, "command");
			std::string sdays = request::findValue(&req, "days");
			std::string slevel = request::findValue(&req, "level");	//in percentage
			std::string smday = "0";
			std::string smonth = "0";
			std::string soccurence = "0";
			std::string persistent = request::findValue(&req, "persistent");
			if ((idx.empty()) || (active.empty()) || (stimertype.empty()) || (shour.empty()) || (smin.empty()) || (randomness.empty()) || (scmd.empty()) || (sdays.empty()))
				return;

			unsigned char iTimerType = atoi(stimertype.c_str());
			int timer_plan = (persistent == "true") ? 9999 : m_sql.m_ActiveTimerPlan;

			time_t now = mytime(nullptr);
			struct tm tm1;
			localtime_r(&now, &tm1);
			int Year = tm1.tm_year + 1900;
			int Month = tm1.tm_mon + 1;
			int Day = tm1.tm_mday;

			if (iTimerType == TTYPE_FIXEDDATETIME)
			{
				if (sdate.size() == 10)
				{
					Year = atoi(sdate.substr(0, 4).c_str());
					Month = atoi(sdate.substr(5, 2).c_str());
					Day = atoi(sdate.substr(8, 2).c_str());
				}
			}
			else if (iTimerType == TTYPE_MONTHLY)
			{
				smday = request::findValue(&req, "mday");
				if (smday.empty())
					return;
			}
			else if (iTimerType == TTYPE_MONTHLY_WD)
			{
				soccurence = request::findValue(&req, "occurence");
				if (soccurence.empty())
					return;
			}
			else if (iTimerType == TTYPE_YEARLY)
			{
				smday = request::findValue(&req, "mday");
				smonth = request::findValue(&req, "month");
				if ((smday.empty()) || (smonth.empty()))
					return;
			}
			else if (iTimerType == TTYPE_YEARLY_WD)
			{
				smonth = request::findValue(&req, "month");
				soccurence = request::findValue(&req, "occurence");
				if ((smonth.empty()) || (soccurence.empty()))
					return;
			}

			unsigned char hour = atoi(shour.c_str());
			unsigned char min = atoi(smin.c_str());
			unsigned char icmd = atoi(scmd.c_str());
			int days = atoi(sdays.c_str());
			unsigned char level = atoi(slevel.c_str());
			int mday = atoi(smday.c_str());
			int month = atoi(smonth.c_str());
			int occurence = atoi(soccurence.c_str());
			root["status"] = "OK";
			root["title"] = "UpdateSceneTimer";
			m_sql.safe_query(
				"UPDATE SceneTimers SET Active=%d, [Date]='%04d-%02d-%02d', Time='%02d:%02d', Type=%d, UseRandomness=%d, Cmd=%d, Level=%d, Days=%d, MDay=%d, Month=%d, Occurence=%d, TimerPlan=%d WHERE (ID == '%q')",
				(active == "true") ? 1 : 0,
				Year, Month, Day,
				hour, min,
				iTimerType,
				(randomness == "true") ? 1 : 0,
				icmd,
				level,
				days,
				mday,
				month,
				occurence,
				timer_plan,
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DeleteSceneTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DeleteSceneTimer";
			m_sql.safe_query(
				"DELETE FROM SceneTimers WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_EnableSceneTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "EnableSceneTimer";
			m_sql.safe_query(
				"UPDATE SceneTimers SET Active=1 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_DisableSceneTimer(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "DisableSceneTimer";
			m_sql.safe_query(
				"UPDATE SceneTimers SET Active=0 WHERE (ID == '%q')",
				idx.c_str()
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_ClearSceneTimers(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			int timer_plan = m_sql.m_ActiveTimerPlan;

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			root["status"] = "OK";
			root["title"] = "ClearSceneTimer";
			m_sql.safe_query(
				"DELETE FROM SceneTimers WHERE (SceneRowID == '%q') AND ((TimerPlan==%d) OR (TimerPlan==9999))",
				idx.c_str(),
				timer_plan
			);
			m_mainworker.m_scheduler.ReloadSchedules();
		}

		void CWebServer::Cmd_GetTimerPlans(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}
			root["status"] = "OK";
			root["title"] = "GetTimerPlans";
			std::vector<std::vector<std::string> > result;
			result = m_sql.safe_query("SELECT a.ID, a.Name, b.nValue FROM TimerPlans a, Preferences b WHERE b.Key = 'ActiveTimerPlan' ORDER BY Name COLLATE NOCASE ASC");
			if (!result.empty())
			{
				int ii = 0;
				for (const auto& sd : result)
				{
					root["result"][ii]["idx"] = atoi(sd[0].c_str());
					root["result"][ii]["Name"] = sd[1];
					root["result"][ii]["Active"] = (sd[0] == sd[2]);
					ii++;
				}
			}
		}

		void CWebServer::Cmd_AddTimerPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if (name.empty())
			{
				session.reply_status = reply::bad_request;
				return;
			}
			root["status"] = "OK";
			root["title"] = "AddTimerPlan";
			m_sql.safe_query("INSERT INTO TimerPlans (Name) VALUES ('%q')", name.c_str());
		}

		void CWebServer::Cmd_UpdateTimerPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if (name.empty())
			{
				session.reply_status = reply::bad_request;
				return;
			}

			root["status"] = "OK";
			root["title"] = "UpdateTimerPlan";

			m_sql.safe_query("UPDATE TimerPlans SET Name='%q' WHERE (ID == '%q')", name.c_str(), idx.c_str());
		}

		void CWebServer::Cmd_DeleteTimerPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			int iPlan = atoi(idx.c_str());
			if (iPlan < 1)
				return;

			root["status"] = "OK";
			root["title"] = "DeletePlan";

			m_sql.safe_query("DELETE FROM Timers WHERE (TimerPlan == '%q')", idx.c_str());
			m_sql.safe_query("DELETE FROM SceneTimers WHERE (TimerPlan == '%q')", idx.c_str());
			m_sql.safe_query("DELETE FROM SetpointTimers WHERE (TimerPlan == '%q')", idx.c_str());

			m_sql.safe_query("DELETE FROM TimerPlans WHERE (ID == '%q')", idx.c_str());

			if (m_sql.m_ActiveTimerPlan == iPlan)
			{
				//Set active timer plan to default
				m_sql.UpdatePreferencesVar("ActiveTimerPlan", 0);
				m_sql.m_ActiveTimerPlan = 0;
				m_mainworker.m_scheduler.ReloadSchedules();
			}
		}

		void CWebServer::Cmd_DuplicateTimerPlan(WebEmSession& session, const request& req, Json::Value& root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; //Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			std::string name = HTMLSanitizer::Sanitize(request::findValue(&req, "name"));
			if (
				(name.empty())
				)
				return;

			root["status"] = "OK";
			root["title"] = "DuplicateTimerPlan";

			m_sql.safe_query("INSERT INTO TimerPlans (Name) VALUES ('%q')", name.c_str());

			std::vector<std::vector<std::string> > result;

			result = m_sql.safe_query("SELECT MAX(ID) FROM TimerPlans WHERE (Name=='%q')", name.c_str());
			if (!result.empty())
			{
				std::string nID = result[0][0];

				//Normal Timers
				result = m_sql.safe_query("SELECT Active, DeviceRowID, Time, Type, Cmd, Level, Days, UseRandomness, [Date], MDay, Month, Occurence, Color FROM Timers WHERE (TimerPlan==%q) ORDER BY ID", idx.c_str());
				for (const auto& sd : result)
				{
					m_sql.safe_query(
						"INSERT INTO Timers (Active, DeviceRowID, Time, Type, Cmd, Level, Days, UseRandomness, [Date], MDay, Month, Occurence, Color, TimerPlan) VALUES (%q, %q, '%q', %q, %q, %q, %q, %q, '%q', %q, %q, %q, '%q', %q)",
						sd[0].c_str(),
						sd[1].c_str(),
						sd[2].c_str(),
						sd[3].c_str(),
						sd[4].c_str(),
						sd[5].c_str(),
						sd[6].c_str(),
						sd[7].c_str(),
						sd[8].c_str(),
						sd[9].c_str(),
						sd[10].c_str(),
						sd[11].c_str(),
						sd[12].c_str(),
						nID.c_str()
					);
				}
				//Scene Timers
				result = m_sql.safe_query("SELECT Active, SceneRowID, Time, Type, Cmd, Level, Days, UseRandomness, [Date], Month, MDay, Occurence FROM SceneTimers WHERE (TimerPlan==%q) ORDER BY ID", idx.c_str());
				for (const auto& sd : result)
				{
					m_sql.safe_query(
						"INSERT INTO SceneTimers (Active, SceneRowID, Time, Type, Cmd, Level, Days, UseRandomness, [Date], Month, MDay, Occurence, TimerPlan) VALUES (%q, %q, '%q', %q, %q, %q, %q, %q, '%q', %q, %q, %q, %q)",
						sd[0].c_str(),
						sd[1].c_str(),
						sd[2].c_str(),
						sd[3].c_str(),
						sd[4].c_str(),
						sd[5].c_str(),
						sd[6].c_str(),
						sd[7].c_str(),
						sd[8].c_str(),
						sd[9].c_str(),
						sd[10].c_str(),
						sd[11].c_str(),
						nID.c_str()
					);
				}
				//Setpoint Timers
				result = m_sql.safe_query("SELECT Active, DeviceRowID, [Date], Time, Type, Temperature, Days, Month, MDay, Occurence FROM SetpointTimers WHERE (TimerPlan==%q) ORDER BY ID", idx.c_str());
				for (const auto& sd : result)
				{
					m_sql.safe_query(
						"INSERT INTO SetpointTimers (Active, DeviceRowID, [Date], Time, Type, Temperature, Days, Month, MDay, Occurence, TimerPlan) VALUES (%q, %q, '%q', '%q', %q, %q, %q, %q, %q, %q, %q)",
						sd[0].c_str(),
						sd[1].c_str(),
						sd[2].c_str(),
						sd[3].c_str(),
						sd[4].c_str(),
						sd[5].c_str(),
						sd[6].c_str(),
						sd[7].c_str(),
						sd[8].c_str(),
						sd[9].c_str(),
						nID.c_str()
					);
				}
			}
		}
	} // namespace server
} // namespace http
