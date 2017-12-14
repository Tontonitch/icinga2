/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2017 Icinga Development Team (https://www.icinga.com/)  *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "icinga/compatutility.hpp"
#include "icinga/checkcommand.hpp"
#include "icinga/eventcommand.hpp"
#include "icinga/notificationcommand.hpp"
#include "icinga/pluginutility.hpp"
#include "icinga/service.hpp"
#include "base/utility.hpp"
#include "base/configtype.hpp"
#include "base/objectlock.hpp"
#include "base/convert.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/join.hpp>

using namespace icinga;

/* command */
String CompatUtility::GetCommandLine(const Command::Ptr& command)
{
	Value commandLine = command->GetCommandLine();

	String result;
	if (commandLine.IsObjectType<Array>()) {
		Array::Ptr args = commandLine;

		ObjectLock olock(args);
		for (const String& arg : args) {
			// This is obviously incorrect for non-trivial cases.
			result += " \"" + EscapeString(arg) + "\"";
		}
	} else if (!commandLine.IsEmpty()) {
		result = EscapeString(Convert::ToString(commandLine));
	} else {
		result = "<internal>";
	}

	return result;
}

String CompatUtility::GetCommandNamePrefix(const Command::Ptr command)
{
	if (!command)
		return Empty;

	String prefix;
	if (command->GetReflectionType() == CheckCommand::TypeInstance)
		prefix = "check_";
	else if (command->GetReflectionType() == NotificationCommand::TypeInstance)
		prefix = "notification_";
	else if (command->GetReflectionType() == EventCommand::TypeInstance)
		prefix = "event_";

	return prefix;
}

String CompatUtility::GetCommandName(const Command::Ptr command)
{
	if (!command)
		return Empty;

	return GetCommandNamePrefix(command) + command->GetName();
}

/* host */
int CompatUtility::GetHostCurrentState(const Host::Ptr& host)
{
	if (host->GetState() != HostUp && !host->IsReachable())
		return 2; /* hardcoded compat state */

	return host->GetState();
}

String CompatUtility::GetHostStateString(const Host::Ptr& host)
{
	if (host->GetState() != HostUp && !host->IsReachable())
		return "UNREACHABLE"; /* hardcoded compat state */

	return Host::StateToString(host->GetState());
}

int CompatUtility::GetHostNotifyOnDown(const Host::Ptr& host)
{
	unsigned long notification_state_filter = GetCheckableNotificationStateFilter(host);

	if ((notification_state_filter & ServiceCritical) ||
	    (notification_state_filter & ServiceWarning))
		return 1;

	return 0;
}

int CompatUtility::GetHostNotifyOnUnreachable(const Host::Ptr& host)
{
	unsigned long notification_state_filter = GetCheckableNotificationStateFilter(host);

	if (notification_state_filter & ServiceUnknown)
		return 1;

	return 0;
}

/* service */
String CompatUtility::GetCheckableCommandArgs(const Checkable::Ptr& checkable)
{
	CheckCommand::Ptr command = checkable->GetCheckCommand();

	Dictionary::Ptr args = new Dictionary();

	if (command) {
		Host::Ptr host;
		Service::Ptr service;
		tie(host, service) = GetHostService(checkable);
		String command_line = GetCommandLine(command);

		Dictionary::Ptr command_vars = command->GetVars();

		if (command_vars) {
			ObjectLock olock(command_vars);
			for (const Dictionary::Pair& kv : command_vars) {
				String macro = "$" + kv.first + "$"; // this is too simple
				if (command_line.Contains(macro))
					args->Set(kv.first, kv.second);

			}
		}

		Dictionary::Ptr host_vars = host->GetVars();

		if (host_vars) {
			ObjectLock olock(host_vars);
			for (const Dictionary::Pair& kv : host_vars) {
				String macro = "$" + kv.first + "$"; // this is too simple
				if (command_line.Contains(macro))
					args->Set(kv.first, kv.second);
				macro = "$host.vars." + kv.first + "$";
				if (command_line.Contains(macro))
					args->Set(kv.first, kv.second);
			}
		}

		if (service) {
			Dictionary::Ptr service_vars = service->GetVars();

			if (service_vars) {
				ObjectLock olock(service_vars);
				for (const Dictionary::Pair& kv : service_vars) {
					String macro = "$" + kv.first + "$"; // this is too simple
					if (command_line.Contains(macro))
						args->Set(kv.first, kv.second);
					macro = "$service.vars." + kv.first + "$";
					if (command_line.Contains(macro))
						args->Set(kv.first, kv.second);
				}
			}
		}

		String arg_string;
		ObjectLock olock(args);
		for (const Dictionary::Pair& kv : args) {
			arg_string += Convert::ToString(kv.first) + "=" + Convert::ToString(kv.second) + "!";
		}
		return arg_string;
	}

	return Empty;
}

double CompatUtility::GetCheckableCheckInterval(const Checkable::Ptr& checkable)
{
	return checkable->GetCheckInterval() / 60.0;
}

double CompatUtility::GetCheckableRetryInterval(const Checkable::Ptr& checkable)
{
	return checkable->GetRetryInterval() / 60.0;
}

String CompatUtility::GetCheckableCheckPeriod(const Checkable::Ptr& checkable)
{
	TimePeriod::Ptr check_period = checkable->GetCheckPeriod();
	if (check_period)
		return check_period->GetName();
	else
		return "24x7";
}

int CompatUtility::GetCheckableNoMoreNotifications(const Checkable::Ptr& checkable)
{
	if (CompatUtility::GetCheckableNotificationNotificationInterval(checkable) == 0 && !checkable->GetVolatile())
		return 1;

	return 0;
}

int CompatUtility::GetCheckableInNotificationPeriod(const Checkable::Ptr& checkable)
{
	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		TimePeriod::Ptr timeperiod = notification->GetPeriod();

		if (!timeperiod || timeperiod->IsInside(Utility::GetTime()))
			return 1;
	}

	return 0;
}

/* notifications */
int CompatUtility::GetCheckableNotificationLastNotification(const Checkable::Ptr& checkable)
{
	double last_notification = 0.0;
	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		if (notification->GetLastNotification() > last_notification)
			last_notification = notification->GetLastNotification();
	}

	return static_cast<int>(last_notification);
}

int CompatUtility::GetCheckableNotificationNextNotification(const Checkable::Ptr& checkable)
{
	double next_notification = 0.0;
	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		if (next_notification == 0 || notification->GetNextNotification() < next_notification)
			next_notification = notification->GetNextNotification();
	}

	return static_cast<int>(next_notification);
}

int CompatUtility::GetCheckableNotificationNotificationNumber(const Checkable::Ptr& checkable)
{
	int notification_number = 0;
	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		if (notification->GetNotificationNumber() > notification_number)
			notification_number = notification->GetNotificationNumber();
	}

	return notification_number;
}

double CompatUtility::GetCheckableNotificationNotificationInterval(const Checkable::Ptr& checkable)
{
	double notification_interval = -1;

	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		if (notification_interval == -1 || notification->GetInterval() < notification_interval)
			notification_interval = notification->GetInterval();
	}

	if (notification_interval == -1)
		notification_interval = 60;

	return notification_interval / 60.0;
}

int CompatUtility::GetCheckableNotificationTypeFilter(const Checkable::Ptr& checkable)
{
	unsigned long notification_type_filter = 0;

	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		ObjectLock olock(notification);

		notification_type_filter |= notification->GetTypeFilter();
	}

	return notification_type_filter;
}

int CompatUtility::GetCheckableNotificationStateFilter(const Checkable::Ptr& checkable)
{
	unsigned long notification_state_filter = 0;

	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		ObjectLock olock(notification);

		notification_state_filter |= notification->GetStateFilter();
	}

	return notification_state_filter;
}

int CompatUtility::GetCheckableNotifyOnWarning(const Checkable::Ptr& checkable)
{
	if (GetCheckableNotificationStateFilter(checkable) & ServiceWarning)
		return 1;

	return 0;
}

int CompatUtility::GetCheckableNotifyOnCritical(const Checkable::Ptr& checkable)
{
	if (GetCheckableNotificationStateFilter(checkable) & ServiceCritical)
		return 1;

	return 0;
}

int CompatUtility::GetCheckableNotifyOnUnknown(const Checkable::Ptr& checkable)
{
	if (GetCheckableNotificationStateFilter(checkable) & ServiceUnknown)
		return 1;

	return 0;
}

int CompatUtility::GetCheckableNotifyOnRecovery(const Checkable::Ptr& checkable)
{
	if (GetCheckableNotificationTypeFilter(checkable) & NotificationRecovery)
		return 1;

	return 0;
}

int CompatUtility::GetCheckableNotifyOnFlapping(const Checkable::Ptr& checkable)
{
	unsigned long notification_type_filter = GetCheckableNotificationTypeFilter(checkable);

	if ((notification_type_filter & NotificationFlappingStart) ||
	    (notification_type_filter & NotificationFlappingEnd))
		return 1;

	return 0;
}

int CompatUtility::GetCheckableNotifyOnDowntime(const Checkable::Ptr& checkable)
{
	unsigned long notification_type_filter = GetCheckableNotificationTypeFilter(checkable);

	if ((notification_type_filter & NotificationDowntimeStart) ||
	    (notification_type_filter & NotificationDowntimeEnd) ||
	    (notification_type_filter & NotificationDowntimeRemoved))
		return 1;

	return 0;
}

std::set<User::Ptr> CompatUtility::GetCheckableNotificationUsers(const Checkable::Ptr& checkable)
{
	/* Service -> Notifications -> (Users + UserGroups -> Users) */
	std::set<User::Ptr> allUsers;
	std::set<User::Ptr> users;

	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		ObjectLock olock(notification);

		users = notification->GetUsers();

		std::copy(users.begin(), users.end(), std::inserter(allUsers, allUsers.begin()));

		for (const UserGroup::Ptr& ug : notification->GetUserGroups()) {
			std::set<User::Ptr> members = ug->GetMembers();
			std::copy(members.begin(), members.end(), std::inserter(allUsers, allUsers.begin()));
		}
	}

	return allUsers;
}

std::set<UserGroup::Ptr> CompatUtility::GetCheckableNotificationUserGroups(const Checkable::Ptr& checkable)
{
	std::set<UserGroup::Ptr> usergroups;
	/* Service -> Notifications -> UserGroups */
	for (const Notification::Ptr& notification : checkable->GetNotifications()) {
		ObjectLock olock(notification);

		for (const UserGroup::Ptr& ug : notification->GetUserGroups()) {
			usergroups.insert(ug);
		}
	}

	return usergroups;
}

String CompatUtility::GetCheckResultOutput(const CheckResult::Ptr& cr)
{
	if (!cr)
		return Empty;

	String output;

	String raw_output = cr->GetOutput();

	/*
	 * replace semi-colons with colons in output
	 * semi-colon is used as delimiter in various interfaces
	 */
	boost::algorithm::replace_all(raw_output, ";", ":");

	size_t line_end = raw_output.Find("\n");

	return raw_output.SubStr(0, line_end);
}

String CompatUtility::GetCheckResultLongOutput(const CheckResult::Ptr& cr)
{
	if (!cr)
		return Empty;

	String long_output;
	String output;

	String raw_output = cr->GetOutput();

	/*
	 * replace semi-colons with colons in output
	 * semi-colon is used as delimiter in various interfaces
	 */
	boost::algorithm::replace_all(raw_output, ";", ":");

	size_t line_end = raw_output.Find("\n");

	if (line_end > 0 && line_end != String::NPos) {
		long_output = raw_output.SubStr(line_end+1, raw_output.GetLength());
		return EscapeString(long_output);
	}

	return Empty;
}

String CompatUtility::EscapeString(const String& str)
{
	String result = str;
	boost::algorithm::replace_all(result, "\n", "\\n");
	return result;
}

String CompatUtility::UnEscapeString(const String& str)
{
	String result = str;
	boost::algorithm::replace_all(result, "\\n", "\n");
	return result;
}
