/*
 * HttpResponder.cpp
 *
 *  Created on: 14 Apr 2017
 *      Author: David
 */

#include "HttpResponder.h"

#if SUPPORT_HTTP

#include "Network.h"
#include "Socket.h"
#include "GCodes/GCodes.h"
#include "General/IP4String.h"

#define KO_START "rr_"
const size_t KoFirst = 3;

const char* const overflowResponse = "overflow";
const char* const badEscapeResponse = "bad escape";
const char serviceUnavailableResponse[] = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
static_assert(ARRAY_SIZE(serviceUnavailableResponse) <= OUTPUT_BUFFER_SIZE, "OUTPUT_BUFFER_SIZE too small");

const uint32_t HttpReceiveTimeout = 2000;

// Text for a human-readable 404 page
const char* const ErrorPagePart1 =
	"<html>\n"
	"<head>\n"
	"</head>\n"
	"<body>\n"
	"<p style=\"font-size: 16pt; text-align: center; margin-top:50px\">Your Duet rejected the HTTP request: ";

const char* const ErrorPagePart2 =
	"</p>\n"
	"</body>\n";

HttpResponder::HttpResponder(NetworkResponder *n) noexcept : UploadingNetworkResponder(n)
{
}

// Ask the responder to accept this connection, returns true if it did
bool HttpResponder::Accept(Socket *s, NetworkProtocol protocol) noexcept
{
	if (responderState == ResponderState::free && protocol == HttpProtocol)
	{
		responderState = ResponderState::reading;
		skt = s;
		timer = millis();

		// Reset the parse state variables
		clientPointer = 0;
		parseState = HttpParseState::doingCommandWord;
		numCommandWords = 0;
		numQualKeys = 0;
		numHeaderKeys = 0;
		commandWords[0] = clientMessage;

		if (reprap.Debug(moduleWebserver))
		{
			debugPrintf("HTTP connection accepted\n");
		}
		return true;
	}
	return false;
}

// Do some work, returning true if we did anything significant
bool HttpResponder::Spin() noexcept
{
	switch (responderState)
	{
	case ResponderState::free:
		return false;

	case ResponderState::reading:
		{
			bool readSomething = false;
			char c;
			while (skt->ReadChar(c))
			{
				if (CharFromClient(c))
				{
					timer = millis();		// restart the timeout
					return true;
				}
				readSomething = true;
			}

			// Here when we were not able to read a character but we didn't receive a finished message
			if (readSomething)
			{
				timer = millis();			// restart the timeout
				return true;
			}

			if (!skt->CanRead() || millis() - timer >= HttpReceiveTimeout)
			{
				ConnectionLost();
				return true;
			}

			return false;
		}

	case ResponderState::processingRequest:
		ProcessRequest();
		return true;

	case ResponderState::gettingFileInfo:
		(void)SendFileInfo(millis() - startedProcessingRequestAt >= MaxFileInfoGetTime);
		return true;

#if HAS_MASS_STORAGE
	case ResponderState::uploading:
		DoUpload();
		return true;
#endif

	case ResponderState::sending:
		SendData();
		return true;

	default:	// should not happen
		return false;
	}
}

// Process a character from the client
// Rewritten as a state machine by dc42 to increase capability and speed, and reduce RAM requirement.
// On entry:
//  There is space for at least 1 character in clientMessage.
// On return:
//	If we return false:
//		We want more characters. There is space for at least 1 character in clientMessage.
//	If we return true:
//		We have processed the message and sent the reply. No more characters may be read from this message.
// Whenever this calls ProcessMessage:
//	The first line has been split up into words. Variables numCommandWords and commandWords give the number of words we found
//  and the pointers to each word. The second word is treated specially. It is assumed to be a filename followed by an optional
//  qualifier comprising key/value pairs. Both may include %xx escapes, and the qualifier may include + to mean space. We store
//  a pointer to the filename without qualifier in commandWords[1]. We store the qualifier key/value pointers in array 'qualifiers'
//  and the number of them in numQualKeys.
//  The remaining lines have been parsed as header name/value pairs. Pointers to them are stored in array 'headers' and the number
//  of them in numHeaders.
// If one of our arrays is about to overflow, or the message is not in a format we expect, then we call RejectMessage with an
// appropriate error code and string.
bool HttpResponder::CharFromClient(char c) noexcept
{
	switch (parseState)
	{
	case HttpParseState::doingCommandWord:
		switch(c)
		{
		case '\n':
			clientMessage[clientPointer++] = 0;
			++numCommandWords;
			numHeaderKeys = 0;
			headers[0].key = clientMessage + clientPointer;
			parseState = HttpParseState::doingHeaderKey;
			break;
		case '\r':
			break;
		case ' ':
		case '\t':
			clientMessage[clientPointer++] = 0;
			{
				++numCommandWords;
				if (numCommandWords < MaxCommandWords)
				{
					commandWords[numCommandWords] = clientMessage + clientPointer;
					if (numCommandWords == 1)
					{
						parseState = HttpParseState::doingFilename;
					}
				}
				else
				{
					RejectMessage("too many command words");
					return true;
				}
			}
			break;
		default:
			clientMessage[clientPointer++] = c;
			break;
		}
		break;

	case HttpParseState::doingFilename:
		switch(c)
		{
		case '\n':
			clientMessage[clientPointer++] = 0;
			++numCommandWords;
			numQualKeys = 0;
			numHeaderKeys = 0;
			headers[0].key = clientMessage + clientPointer;
			parseState = HttpParseState::doingHeaderKey;
			break;
		case '?':
			clientMessage[clientPointer++] = 0;
			++numCommandWords;
			numQualKeys = 0;
			qualifiers[0].key = clientMessage + clientPointer;
			parseState = HttpParseState::doingQualifierKey;
			break;
		case '%':
			parseState = HttpParseState::doingFilenameEsc1;
			break;
		case '\r':
			break;
		case ' ':
		case '\t':
			clientMessage[clientPointer++] = 0;
			{
				++numCommandWords;
				if (numCommandWords < MaxCommandWords)
				{
					commandWords[numCommandWords] = clientMessage + clientPointer;
					parseState = HttpParseState::doingCommandWord;
				}
				else
				{
					RejectMessage("too many command words");
					return true;
				}
			}
			break;
		default:
			clientMessage[clientPointer++] = c;
			break;
		}
		break;

	case HttpParseState::doingQualifierKey:
		switch(c)
		{
		case '=':
			clientMessage[clientPointer++] = 0;
			qualifiers[numQualKeys].value = clientMessage + clientPointer;
			++numQualKeys;
			parseState = HttpParseState::doingQualifierValue;
			break;
		case '\n':	// key with no value
		case ' ':
		case '\t':
		case '\r':
			// IE11 sometimes puts a trailing '?' at the end of a GET request e.g. "GET /fonts/glyphicons.eot? HTTP/1.1"
			if (numQualKeys == 0 && qualifiers[0].key == clientMessage + clientPointer)
			{
				commandWords[numCommandWords] = clientMessage + clientPointer;	// we have only 2 command words so far, so no need to check numCommandWords here
				parseState = HttpParseState::doingCommandWord;
				break;
			}
			// no break
		case '%':	// none of our keys needs escaping, so treat an escape within a key as an error
		case '&':	// key with no value
			RejectMessage("bad qualifier key");
			return true;
		default:
			clientMessage[clientPointer++] = c;
			break;
		}
		break;

	case HttpParseState::doingQualifierValue:
		switch(c)
		{
		case '\n':
			clientMessage[clientPointer++] = 0;
			qualifiers[numQualKeys].key = clientMessage + clientPointer;	// so that we can read the whole value even if it contains a null
			numHeaderKeys = 0;
			headers[0].key = clientMessage + clientPointer;
			parseState = HttpParseState::doingHeaderKey;
			break;
		case ' ':
		case '\t':
			clientMessage[clientPointer++] = 0;
			qualifiers[numQualKeys].key = clientMessage + clientPointer;	// so that we can read the whole value even if it contains a null
			commandWords[numCommandWords] = clientMessage + clientPointer;
			parseState = HttpParseState::doingCommandWord;
			break;
		case '\r':
			break;
		case '%':
			parseState = HttpParseState::doingQualifierValueEsc1;
			break;
		case '&':
			// Another variable is coming
			clientMessage[clientPointer++] = 0;
			qualifiers[numQualKeys].key = clientMessage + clientPointer;	// so that we can read the whole value even if it contains a null
			if (numQualKeys < MaxQualKeys)
			{
				parseState = HttpParseState::doingQualifierKey;
			}
			else
			{
				RejectMessage("too many keys in qualifier");
				return true;
			}
			break;
		case '+':
			clientMessage[clientPointer++] = ' ';
			break;
		default:
			clientMessage[clientPointer++] = c;
			break;
		}
		break;

	case HttpParseState::doingFilenameEsc1:
	case HttpParseState::doingQualifierValueEsc1:
		if (c >= '0' && c <= '9')
		{
			decodeChar = (c - '0') << 4;
			parseState = (HttpParseState)((int)parseState + 1);
		}
		else if (c >= 'A' && c <= 'F')
		{
			decodeChar = (c - ('A' - 10)) << 4;
			parseState = (HttpParseState)((int)parseState + 1);
		}
		else
		{
			RejectMessage(badEscapeResponse);
			return true;
		}
		break;

	case HttpParseState::doingFilenameEsc2:
	case HttpParseState::doingQualifierValueEsc2:
		if (c >= '0' && c <= '9')
		{
			clientMessage[clientPointer++] = decodeChar | (c - '0');
			parseState = (HttpParseState)((int)parseState - 2);
		}
		else if (c >= 'A' && c <= 'F')
		{
			clientMessage[clientPointer++] = decodeChar | (c - ('A' - 10));
			parseState = (HttpParseState)((int)parseState - 2);
		}
		else
		{
			RejectMessage(badEscapeResponse);
			return true;
		}
		break;

	case HttpParseState::doingHeaderKey:
		switch(c)
		{
		case '\n':
			if (clientMessage + clientPointer == headers[numHeaderKeys].key)	// if the key hasn't started yet, then this is the blank line at the end
			{
				ProcessMessage();
				return true;
			}
			else
			{
				RejectMessage("unexpected newline");
				return true;
			}
			break;
		case '\r':
			break;
		case ':':
			if (numHeaderKeys == MaxHeaders - 1)
			{
				RejectMessage("too many header key-value pairs");
				return true;
			}
			clientMessage[clientPointer++] = 0;
			headers[numHeaderKeys].value = clientMessage + clientPointer;
			++numHeaderKeys;
			parseState = HttpParseState::expectingHeaderValue;
			break;
		default:
			clientMessage[clientPointer++] = c;
			break;
		}
		break;

	case HttpParseState::expectingHeaderValue:
		if (c == ' ' || c == '\t')
		{
			break;		// ignore spaces between header key and value
		}
		parseState = HttpParseState::doingHeaderValue;
		// no break

	case HttpParseState::doingHeaderValue:
		if (c == '\n')
		{
			parseState = HttpParseState::doingHeaderContinuation;
		}
		else if (c != '\r')
		{
			clientMessage[clientPointer++] = c;
		}
		break;

	case HttpParseState::doingHeaderContinuation:
		switch(c)
		{
		case ' ':
		case '\t':
			// It's a continuation of the previous value
			clientMessage[clientPointer++] = c;
			parseState = HttpParseState::doingHeaderValue;
			break;
		case '\n':
			// It's the blank line
			clientMessage[clientPointer] = 0;
			ProcessMessage();
			return true;
		case '\r':
			break;
		default:
			// It's a new key
			if (clientPointer + 3 <= ARRAY_SIZE(clientMessage))
			{
				clientMessage[clientPointer++] = 0;
				headers[numHeaderKeys].key = clientMessage + clientPointer;
				clientMessage[clientPointer++] = c;
				parseState = HttpParseState::doingHeaderKey;
			}
			else
			{
				RejectMessage(overflowResponse);
				return true;
			}
			break;
		}
		break;

	default:
		break;
	}

	if (clientPointer == ARRAY_SIZE(clientMessage))
	{
		RejectMessage(overflowResponse);
		return true;
	}
	return false;
}

// Get the Json response for this command.
// 'value' is null-terminated, but we also pass its length in case it contains embedded nulls, which matters when uploading files.
// Return true if we generated a json response to send, false if we didn't and changed the state instead.
// This may also return true with response == nullptr if we tried to generate a response but ran out of buffers.
bool HttpResponder::GetJsonResponse(const char *_ecv_array request, OutputBuffer *&response, bool& keepOpen) noexcept
{
	keepOpen = false;	// assume we don't want to persist the connection
	const char *parameter;
	if (StringEqualsIgnoreCase(request, "connect") && (parameter = GetKeyValue("password")) != nullptr)
	{
		if (!CheckAuthenticated())
		{
			if (!reprap.CheckPassword(parameter))
			{
				// Wrong password
				response->copy("{\"err\":1}");
				reprap.GetPlatform().MessageF(LogWarn, "HTTP client %s attempted login with incorrect password\n", IP4String(GetRemoteIP()).c_str());
				return true;
			}
			if (!Authenticate())
			{
				// No more HTTP sessions available
				response->copy("{\"err\":2}");
				reprap.GetPlatform().MessageF(LogWarn, "HTTP client %s attempted login but no more sessions available\n", IP4String(GetRemoteIP()).c_str());
				return true;
			}
		}

		// Client has been logged in
		response->printf("{\"err\":0,\"sessionTimeout\":%" PRIu32 ",\"boardType\":\"%s\",\"apiLevel\":%u}",
							HttpSessionTimeout, GetPlatform().GetBoardString(), ApiLevel);
		reprap.GetPlatform().MessageF(LogWarn, "HTTP client %s login succeeded\n", IP4String(GetRemoteIP()).c_str());

		// See if we can update the current RTC date and time
		const char* const timeString = GetKeyValue("time");
		if (timeString != nullptr && !GetPlatform().IsDateTimeSet())
		{
			struct tm timeInfo;
			memset(&timeInfo, 0, sizeof(timeInfo));
			if (SafeStrptime(timeString, "%Y-%m-%dT%H:%M:%S", &timeInfo) != nullptr)
			{
				GetPlatform().SetDateTime(mktime(&timeInfo));
			}
		}
	}
	else if (!CheckAuthenticated())
	{
		RejectMessage("Not authorized", 401);
		return false;
	}
	else if (StringEqualsIgnoreCase(request, "disconnect"))
	{
		response->printf("{\"err\":%d}", (RemoveAuthentication()) ? 0 : 1);
		reprap.GetPlatform().MessageF(LogWarn, "HTTP client %s disconnected\n", IP4String(GetRemoteIP()).c_str());
	}
	else if (StringEqualsIgnoreCase(request, "status"))
	{
		const char *typeString = GetKeyValue("type");
		if (typeString != nullptr)
		{
			// New-style JSON status responses
			int32_t type = StrToI32(typeString);
			if (type < 1 || type > 3)
			{
				type = 1;
			}

			OutputBuffer::ReleaseAll(response);
			response = reprap.GetStatusResponse(type, ResponseSource::HTTP);		// this may return nullptr
		}
		else
		{
			// Deprecated
			OutputBuffer::ReleaseAll(response);
			response = reprap.GetLegacyStatusResponse(1, 0);
		}
	}
	else if (StringEqualsIgnoreCase(request, "gcode"))
	{
		const char *command = GetKeyValue("gcode");
		NetworkGCodeInput * const httpInput = reprap.GetGCodes().GetHTTPInput();
		// If the command is empty, just report the buffer space. This allows rr_gcode to be used to poll the buffer space without using it up.
		if (command != nullptr && command[0] != 0)
		{
			httpInput->Put(HttpMessage, command);
		}
		response->printf("{\"buff\":%u}", httpInput->BufferSpaceLeft());
	}
#if HAS_MASS_STORAGE
	else if (StringEqualsIgnoreCase(request, "upload"))
	{
		response->printf("{\"err\":%d}", (uploadError) ? 1 : 0);
	}
	else if (StringEqualsIgnoreCase(request, "delete") && (parameter = GetKeyValue("name")) != nullptr)
	{
		const bool ok = MassStorage::Delete(parameter, false);
		response->printf("{\"err\":%d}", (ok) ? 0 : 1);
	}
	else if (StringEqualsIgnoreCase(request, "filelist") && (parameter = GetKeyValue("dir")) != nullptr)
	{
		OutputBuffer::ReleaseAll(response);
		const char* const firstVal = GetKeyValue("first");
		const unsigned int startAt = (firstVal == nullptr) ? 0 : StrToU32(firstVal);
		response = reprap.GetFilelistResponse(parameter, startAt);		// this may return nullptr
	}
	else if (StringEqualsIgnoreCase(request, "files"))
	{
		OutputBuffer::ReleaseAll(response);
		const char* dir = GetKeyValue("dir");
		if (dir == nullptr)
		{
			dir = Platform::GetGCodeDir();
		}
		const char* const firstVal = GetKeyValue("first");
		const unsigned int startAt = (firstVal == nullptr) ? 0 : StrToU32(firstVal);
		const char* const flagDirsVal = GetKeyValue("flagDirs");
		const bool flagDirs = flagDirsVal != nullptr && StrToU32(flagDirsVal) == 1;
		response = reprap.GetFilesResponse(dir, startAt, flagDirs);				// this may return nullptr
	}
	else if (StringEqualsIgnoreCase(request, "move"))
	{
		const char* const oldVal = GetKeyValue("old");
		const char* const newVal = GetKeyValue("new");
		bool success = false;
		if (oldVal != nullptr && newVal != nullptr)
		{
			const bool deleteExisting = StringEqualsIgnoreCase(GetKeyValue("deleteexisting"), "yes");
			success = MassStorage::Rename(oldVal, newVal, deleteExisting, false);
		}
		response->printf("{\"err\":%d}", (success) ? 0 : 1);
	}
	else if (StringEqualsIgnoreCase(request, "mkdir"))
	{
		const char* const dirVal = GetKeyValue("dir");
		bool success = false;
		if (dirVal != nullptr)
		{
			success = MassStorage::MakeDirectory(dirVal, false);
		}
		response->printf("{\"err\":%d}", (success) ? 0 : 1);
	}
	else if (StringEqualsIgnoreCase(request, "thumbnail"))
	{
		const char* const nameVal = GetKeyValue("name");
		const char* const offsetVal = GetKeyValue("offset");
		FilePosition offset;
		if (nameVal != nullptr && offsetVal != nullptr && (offset = StrToU32(offsetVal)) != 0)
		{
			OutputBuffer::ReleaseAll(response);
			response = reprap.GetThumbnailResponse(nameVal, offset, false);
		}
		else
		{
			response->copy("{\"err\":1}");
		}
	}
#else
	else if (	StringEqualsIgnoreCase(request, "upload")
			 || StringEqualsIgnoreCase(request, "delete")
			 || StringEqualsIgnoreCase(request, "filelist")
			 || StringEqualsIgnoreCase(request, "files")
			 || StringEqualsIgnoreCase(request, "move")
			 || StringEqualsIgnoreCase(request, "mkdir")
			 || StringEqualsIgnoreCase(request, "thumbnail")
			)
	{
		response->copy("{err:1}");
	}
#endif
	else if (StringEqualsIgnoreCase(request, "fileinfo"))
	{
		const char* const nameVal = GetKeyValue("name");
		if (nameVal != nullptr)
		{
			// Regular rr_fileinfo?name=xxx call
			filenameBeingProcessed.copy(nameVal);
		}
		else
		{
			// Simple rr_fileinfo call to get info about the file being printed
			filenameBeingProcessed.Clear();
		}
		responderState = ResponderState::gettingFileInfo;
		return false;
	}
#if SUPPORT_OBJECT_MODEL
	else if (StringEqualsIgnoreCase(request, "model"))
	{
		OutputBuffer::ReleaseAll(response);
		const char *const filterVal = GetKeyValue("key");
		const char *const flagsVal = GetKeyValue("flags");
		response = reprap.GetModelResponse(nullptr, filterVal, flagsVal);
	}
#endif
	else if (StringEqualsIgnoreCase(request, "config"))
	{
		OutputBuffer::ReleaseAll(response);
		response = reprap.GetConfigResponse();
	}
	else
	{
		RejectMessage("Unknown request", 500);
		return false;
	}

	return true;
}

const char* HttpResponder::GetKeyValue(const char *key) const noexcept
{
	for (size_t i = 0; i < numQualKeys; ++i)
	{
		if (StringEqualsIgnoreCase(qualifiers[i].key, key))
		{
			return qualifiers[i].value;
		}
	}
	return nullptr;
}

// Called to process a FileInfo request, which may take several calls
// Return true if complete
bool HttpResponder::SendFileInfo(bool quitEarly) noexcept
{
	OutputBuffer *jsonResponse = nullptr;
	bool gotFileInfo = (reprap.GetFileInfoResponse(filenameBeingProcessed.c_str(), jsonResponse, quitEarly) != GCodeResult::notFinished);
	if (gotFileInfo)
	{
		// Got it - send the response now
		outBuf->copy(	"HTTP/1.1 200 OK\r\n"
						"Cache-Control: no-cache, no-store, must-revalidate\r\n"
						"Pragma: no-cache\r\n"
						"Expires: 0\r\n"
						"Content-Type: application/json\r\n"
					);
		outBuf->catf("Content-Length: %u\r\n", (jsonResponse != nullptr) ? jsonResponse->Length() : 0);
		AddCorsHeader();
		outBuf->cat("Connection: close\r\n\r\n");
		outBuf->Append(jsonResponse);
		if (outBuf->HadOverflow())
		{
			OutputBuffer::ReleaseAll(outBuf);
			ReportOutputBufferExhaustion(__FILE__, __LINE__);
			gotFileInfo = false;
		}
		else
		{
			filenameBeingProcessed.Clear();
			Commit();
		}
	}
	return gotFileInfo;
}

// Authenticate current IP and return true on success
bool HttpResponder::Authenticate() noexcept
{
	if (CheckAuthenticated())
	{
		return true;
	}

	if (numSessions < MaxHttpSessions)
	{
		sessions[numSessions].ip = GetRemoteIP();
		sessions[numSessions].lastQueryTime = millis();
		sessions[numSessions].isPostUploading = false;
		numSessions++;
		return true;
	}
	return false;
}

// Check and update the authentication
bool HttpResponder::CheckAuthenticated() noexcept
{
	const IPAddress remoteIP = GetRemoteIP();
	for (size_t i = 0; i < numSessions; i++)
	{
		if (sessions[i].ip == remoteIP)
		{
			sessions[i].lastQueryTime = millis();
			return true;
		}
	}
	return false;
}

bool HttpResponder::RemoveAuthentication() noexcept
{
	const IPAddress remoteIP = skt->GetRemoteIP();
	for (size_t i = numSessions; i != 0; )
	{
		--i;
		if (sessions[i].ip == remoteIP)
		{
			if (sessions[i].isPostUploading)
			{
				// Don't allow sessions with active POST uploads to be removed
				return false;
			}

			RemoveSession(i);
			return true;
		}
	}
	return false;
}

/*static*/ void HttpResponder::RemoveSession(size_t sessionToRemove) noexcept
{
	if (sessionToRemove < numSessions)
	{
		--numSessions;
		for (size_t k = sessionToRemove; k < numSessions; ++k)
		{
			sessions[k] = sessions[k + 1];
		}
	}
}

void HttpResponder::SendFile(const char *_ecv_array nameOfFileToSend, bool isWebFile) noexcept
{
#if HAS_MASS_STORAGE
	FileStore *fileToSend = nullptr;
	bool zip = false;

	if (isWebFile)
	{
		if (nameOfFileToSend[0] == '/')
		{
			++nameOfFileToSend;						// all web files are relative to the /www folder, so remove the leading '/'
		}

		// If we are asked to return the root, return the index file
		if (nameOfFileToSend[0] == 0)
		{
			nameOfFileToSend = INDEX_PAGE_FILE;
		}

		// Check that the length of the filename requested is short enough for CombineName not to generate an error message before we try to open it.
		// We used to report a possible virus attack in this case, but that sometimes leads to false warnings because of OCSP requests from AV programs,
		// or file download requests after IP address changes
		if (strlen(nameOfFileToSend) <= MaxExpectedWebDirFilenameLength)
		{
			for (;;)
			{
				// Try to open a gzipped version of the file first
				if (!StringEndsWithIgnoreCase(nameOfFileToSend, ".gz"))
				{
					static_assert(MaxExpectedWebDirFilenameLength + 3 <= MaxFilenameLength);			// this ensures that we can append '.gz' to the filename without overflow
					String<MaxFilenameLength> nameBuf;
					nameBuf.copy(nameOfFileToSend);
					nameBuf.cat(".gz");
					fileToSend = GetPlatform().OpenFile(Platform::GetWebDir(), nameBuf.c_str(), OpenMode::read);
					if (fileToSend != nullptr)
					{
						zip = true;
						break;
					}
				}

				// That failed, so try to open the normal version of the file
				fileToSend = GetPlatform().OpenFile(Platform::GetWebDir(), nameOfFileToSend, OpenMode::read);
				if (fileToSend != nullptr)
				{
					break;
				}

				if (StringEqualsIgnoreCase(nameOfFileToSend, INDEX_PAGE_FILE))
				{
					nameOfFileToSend = OLD_INDEX_PAGE_FILE;			// the index file wasn't found, so try the old one
				}
				else if (!strchr(nameOfFileToSend, '.'))			// if we were asked to return a file without a '.' in the name, return the index page
				{
					nameOfFileToSend = INDEX_PAGE_FILE;
				}
				else
				{
					break;
				}
			}
		}

		// If we still couldn't find the file and it was an HTML file, return the 404 error page
		if (fileToSend == nullptr && (StringEndsWithIgnoreCase(nameOfFileToSend, ".html") || StringEndsWithIgnoreCase(nameOfFileToSend, ".htm")))
		{
			nameOfFileToSend = FOUR04_PAGE_FILE;
			fileToSend = GetPlatform().OpenFile(Platform::GetWebDir(), nameOfFileToSend, OpenMode::read);
		}

		if (fileToSend == nullptr)
		{
			RejectMessage("page not found<br>Check that the SD card is mounted and has the correct files in its /www folder", 404);
			return;
		}
	}
	else
	{
		fileToSend = GetPlatform().OpenFile(FS_PREFIX, nameOfFileToSend, OpenMode::read);
		if (fileToSend == nullptr)
		{
			RejectMessage("file not found", 404);
			return;
		}
	}

	fileBeingSent = fileToSend;
	outBuf->copy("HTTP/1.1 200 OK\r\n");

	// Don't cache files served by rr_download
	if (!isWebFile)
	{
		outBuf->cat(	"Cache-Control: no-cache, no-store, must-revalidate\r\n"
						"Pragma: no-cache\r\n"
						"Expires: 0\r\n"
					);
		AddCorsHeader();
	}

	const char* contentType;
	if (StringEndsWithIgnoreCase(nameOfFileToSend, ".png"))
	{
		contentType = "image/png";
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".ico"))
	{
		contentType = "image/x-icon";
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".js"))
	{
		contentType = "application/javascript";
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".css"))
	{
		contentType = "text/css";
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".htm") || StringEndsWithIgnoreCase(nameOfFileToSend, ".html"))
	{
		contentType = "text/html";
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".zip"))
	{
		contentType = "application/zip";
		// Don't set zip true here, the content-encoding isn't gzip
	}
	else if (StringEndsWithIgnoreCase(nameOfFileToSend, ".g") || StringEndsWithIgnoreCase(nameOfFileToSend, ".gc") || StringEndsWithIgnoreCase(nameOfFileToSend, ".gcode"))
	{
		contentType = "text/plain";
	}
	else
	{
		contentType = "application/octet-stream";
	}
	outBuf->catf("Content-Type: %s\r\n", contentType);

	if (zip)
	{
		outBuf->cat("Content-Encoding: gzip\r\n");
	}

	outBuf->catf("Content-Length: %lu\r\n", fileToSend->Length());
	outBuf->cat("Connection: close\r\n\r\n");
	Commit();
#else
	RejectMessage("file not found", 404);
#endif
}

void HttpResponder::SendGCodeReply() noexcept
{
	{
		// Do we need to keep the G-Code reply for other clients?
		bool clearReply = false;
		MutexLocker Lock(gcodeReplyMutex);

		if (!gcodeReply.IsEmpty())
		{
			clientsServed++;
			if (clientsServed < numSessions)
			{
				// Yes - make sure the Network class doesn't discard its buffers yet
				// NB: This must happen here, because NetworkTransaction::Write() might already release OutputBuffers
				gcodeReply.IncreaseReferences(1);
			}
			else
			{
				// No - clean up again later
				clearReply = true;
			}

			if (reprap.Debug(moduleWebserver))
			{
				GetPlatform().MessageF(UsbMessage, "Sending G-Code reply to HTTP client %d of %d (length %u)\n", clientsServed, numSessions, gcodeReply.DataLength());
			}
		}

		// Send the whole G-Code reply as plain text to the client
		outBuf->copy(	"HTTP/1.1 200 OK\r\n"
						"Cache-Control: no-cache, no-store, must-revalidate\r\n"
						"Pragma: no-cache\r\n"
						"Expires: 0\r\n"
						"Content-Type: text/plain\r\n"
					);
		outBuf->catf("Content-Length: %u\r\n", gcodeReply.DataLength());
		AddCorsHeader();
		outBuf->cat("Connection: close\r\n\r\n");
		outStack.Append(gcodeReply);

		// Possibly clean up the G-code reply once again
		if (clearReply)
		{
			gcodeReply.Clear();
		}
	}

	Commit();
}

// Send a JSON response to the current command. outBuf is non-null on entry.
void HttpResponder::SendJsonResponse(const char *_ecv_array command) noexcept
{
	// Try to authorise the user automatically to retain compatibility with the old web interface
	if (!CheckAuthenticated() && reprap.NoPasswordSet())
	{
		Authenticate();
	}

	// Update the authentication status and try to handle "text/plain" requests here
	if (CheckAuthenticated())
	{
		if (StringEqualsIgnoreCase(command, "reply"))			// rr_reply
		{
			SendGCodeReply();
			return;
		}

#if HAS_MASS_STORAGE
		if (StringEqualsIgnoreCase(command, "download"))
		{
			const char* const filename = GetKeyValue("name");
			if (filename != nullptr)
			{
				SendFile(filename, false);
				return;
			}
		}
#endif
	}

	// Try to process a request for JSON responses
	OutputBuffer *jsonResponse;
	bool mayKeepOpen;
	if (OutputBuffer::Allocate(jsonResponse))
	{
		const bool gotResponse = GetJsonResponse(command, jsonResponse, mayKeepOpen);
		if (!gotResponse)
		{
			// GetJsonResponse() changed the state instead of returning a response
			OutputBuffer::ReleaseAll(jsonResponse);
			return;
		}
		if (jsonResponse != nullptr && jsonResponse->HadOverflow())
		{
			// The response is incomplete because we ran out of buffers
			OutputBuffer::ReleaseAll(jsonResponse);
		}
	}

	if (jsonResponse == nullptr)
	{
		// We ran out of buffers at some point.
		// DC 2020-05-05: we no longer retry or discard responses if there are no buffers available, instead we return a 503 error immediately
		ReportOutputBufferExhaustion(__FILE__, __LINE__);

		// We know that we have an output buffer, but it may be too short to send a long reply, so send a short one
		outBuf->copy(serviceUnavailableResponse);
		Commit(ResponderState::free, false);
		return;
	}

	// Send the JSON response
	bool keepOpen = false;
	if (mayKeepOpen)
	{
		// Check that the browser wants to persist the connection too
		for (size_t i = 0; i < numHeaderKeys; ++i)
		{
			if (StringEqualsIgnoreCase(headers[i].key, "Connection"))
			{
				// Comment out the following line to disable persistent connections
				keepOpen = StringEqualsIgnoreCase(headers[i].value, "keep-alive");
				break;
			}
		}
	}

	// Note that when using RTOS the following response should preferably be small enough to fit in a single buffer.
	// This is because the current task may get suspended e.g. when reading from SD card to build a file list,
	// so other tasks may allocate buffers meanwhile, and the previous mechanism for ensuring that there is sufficient
	// buffer space remaining don't work.
	// This response is currently about 230 bytes long in the worst case.
	outBuf->copy(	"HTTP/1.1 200 OK\r\n"
					"Cache-Control: no-cache, no-store, must-revalidate\r\n"
					"Pragma: no-cache\r\n"
					"Expires: 0\r\n"
					"Content-Type: application/json\r\n"
				);
	const unsigned int replyLength = (jsonResponse != nullptr) ? jsonResponse->Length() : 0;
	outBuf->catf("Content-Length: %u\r\n", replyLength);
	AddCorsHeader();
	outBuf->catf("Connection: %s\r\n\r\n", keepOpen ? "keep-alive" : "close");
	outBuf->Append(jsonResponse);

	if (outBuf->HadOverflow())
	{
		// We ran out of buffers at some point.
		// DC 2020-05-05: we no longer retry or discard responses if there are no buffers available, instead we return a 503 error immediately
		ReportOutputBufferExhaustion(__FILE__, __LINE__);

		// We know that we have an output buffer, but it may be too short to send a long reply, so send a short one
		outBuf->copy(serviceUnavailableResponse);
		Commit(ResponderState::free, false);
		return;
	}

	// Here if everything is OK
	Commit(keepOpen ? ResponderState::reading : ResponderState::free, false);
	if (reprap.Debug(moduleWebserver))
	{
		debugPrintf("Sending JSON reply, length %u\n", replyLength);
	}
}

// Process the message received. We have reached the end of the headers.
void HttpResponder::ProcessMessage() noexcept
{
	if (reprap.Debug(moduleWebserver))
	{
		Platform& p = GetPlatform();
		p.Message(UsbMessage, "HTTP req, command words {");
		for (size_t i = 0; i < numCommandWords; ++i)
		{
			p.MessageF(UsbMessage, " %s", commandWords[i]);
		}
		p.Message(UsbMessage, " }, parameters {");

		for (size_t i = 0; i < numQualKeys; ++i)
		{
			p.MessageF(UsbMessage, " %s=%s", qualifiers[i].key, qualifiers[i].value);
		}
		p.Message(UsbMessage, " }\n");
	}

	responderState = ResponderState::processingRequest;
	startedProcessingRequestAt = millis();
}

// Process the message received. We have reached the end of the headers.
void HttpResponder::ProcessRequest() noexcept
{
	if (numCommandWords < 2)
	{
		RejectMessage("too few command words");
		return;
	}

	// Reserve an output buffer before we process the request, or we won't be able to reply
	if (outBuf != nullptr || OutputBuffer::Allocate(outBuf))
	{
		if (StringEqualsIgnoreCase(commandWords[0], "GET"))
		{
			if (StringStartsWith(commandWords[1], KO_START))
			{
				SendJsonResponse(commandWords[1] + KoFirst);
			}
			else if (commandWords[1][0] == '/' && StringStartsWith(commandWords[1] + 1, KO_START))
			{
				SendJsonResponse(commandWords[1] + 1 + KoFirst);
			}
			else
			{
				SendFile(commandWords[1], true);
			}
			return;
		}

		if (StringEqualsIgnoreCase(commandWords[0], "OPTIONS"))
		{
			outBuf->copy(	"HTTP/1.1 204 No Content\r\n"
							"Allow: OPTIONS, GET, POST\r\n"
							"Cache-Control: no-cache, no-store, must-revalidate\r\n"
							"Pragma: no-cache\r\n"
							"Expires: 0\r\n"
							"Content-Length: 0\r\n"
						);
			if (reprap.GetNetwork().GetCorsSite() != nullptr)
			{
				outBuf->catf("Access-Control-Allow-Headers: Content-Type\r\n");
				AddCorsHeader();
			}
			outBuf->cat("\r\n");
			if (outBuf->HadOverflow())
			{
				OutputBuffer::ReleaseAll(outBuf);
				ReportOutputBufferExhaustion(__FILE__, __LINE__);
			}
			else
			{
				Commit();
			}
			return;
		}

		if (CheckAuthenticated() && StringEqualsIgnoreCase(commandWords[0], "POST"))
		{
#if HAS_MASS_STORAGE
			const bool isUploadRequest = (StringEqualsIgnoreCase(commandWords[1], KO_START "upload"))
									  || (commandWords[1][0] == '/' && StringEqualsIgnoreCase(commandWords[1] + 1, KO_START "upload"));
			if (isUploadRequest)
			{
				const char* const filename = GetKeyValue("name");
				if (filename != nullptr)
				{
					// See how many bytes we expect to read
					bool contentLengthFound = false;
					for (size_t i = 0; i < numHeaderKeys; i++)
					{
						if (StringEqualsIgnoreCase(headers[i].key, "Content-Length"))
						{
							postFileLength = StrToU32(headers[i].value);
							contentLengthFound = true;
							break;
						}
					}

					// Start POST file upload
					if (!contentLengthFound)
					{
						RejectMessage("invalid POST upload request");
						return;
					}

					// Try to get the expected CRC
					const char* const expectedCrc = GetKeyValue("crc32");
					postFileGotCrc = (expectedCrc != nullptr);
					if (postFileGotCrc)
					{
						postFileExpectedCrc = StrHexToU32(expectedCrc, nullptr);
					}

					// Start a new file upload
					if (!StartUpload(FS_PREFIX, filename, (postFileGotCrc) ? OpenMode::writeWithCrc : OpenMode::write, postFileLength))
					{
						RejectMessage("could not create file");
						return;
					}

					// Try to get the last modified file date and time
					const char* const lastModifiedString = GetKeyValue("time");
					if (lastModifiedString != nullptr)
					{
						struct tm timeInfo;
						memset(&timeInfo, 0, sizeof(timeInfo));
						if (SafeStrptime(lastModifiedString, "%Y-%m-%dT%H:%M:%S", &timeInfo) != nullptr)
						{
							fileLastModified  = mktime(&timeInfo);
						}
						else
						{
							fileLastModified = 0;
						}
					}
					else
					{
						fileLastModified = 0;
					}

					if (reprap.Debug(moduleWebserver))
					{
						GetPlatform().MessageF(UsbMessage, "Start uploading file %s length %lu\n", filename, postFileLength);
					}
					uploadedBytes = 0;

					// Keep track of the connection that is now uploading
					const IPAddress remoteIP = GetRemoteIP();
					const uint16_t remotePort = skt->GetRemotePort();
					for (size_t i = 0; i < numSessions; i++)
					{
						if (sessions[i].ip == remoteIP)
						{
							sessions[i].postPort = remotePort;
							sessions[i].isPostUploading = true;
							break;
						}
					}
					return;
				}
			}
			RejectMessage("only rr_upload is supported for POST requests");
#else
			RejectMessage("POST requests are not supported");
#endif
		}
		else
		{
			RejectMessage("Unknown message type or not authenticated");
		}
	}
	else
	{
		// No output buffers available. Ideally we would wait for one with timeout. For now we just quit.
		responderState = ResponderState::free;
	}
}

// Reject the current message
void HttpResponder::RejectMessage(const char *_ecv_array response, unsigned int code) noexcept
{
	if (reprap.Debug(moduleWebserver))
	{
		GetPlatform().MessageF(UsbMessage, "Webserver: rejecting message with: %u %s\n", code, response);
	}

	if (outBuf != nullptr || OutputBuffer::Allocate(outBuf))
	{
		outBuf->printf("HTTP/1.1 %u %s\r\n"
					   "Connection: close\r\n", code, response);
		AddCorsHeader();
		outBuf->catf("\r\n%s%s%s", ErrorPagePart1, response, ErrorPagePart2);
		Commit();
	}
	else
	{
		// No output buffers available. Ideally we would wait for one with timeout. For now we just quit.
		responderState = ResponderState::free;
	}
}

#if HAS_MASS_STORAGE

// This function overrides the one in class NetworkResponder.
// It tries to process a chunk of uploaded data and changes the state if finished.
void HttpResponder::DoUpload() noexcept
{
	const uint8_t *buffer;
	size_t len;
	if (skt->ReadBuffer(buffer, len))
	{
		(void)CheckAuthenticated();							// uploading may take a long time, so make sure the requester IP is not timed out
		timer = millis();									// reset the timer

		const bool ok = dummyUpload || fileBeingUploaded.Write(buffer, len);
		skt->Taken(len);
		uploadedBytes += len;

		if (!ok)
		{
			uploadError = true;
			GetPlatform().Message(ErrorMessage, "HTTP: could not write upload data\n");
			CancelUpload();
			SendJsonResponse("upload");
			return;
		}
	}
	else if (!skt->CanRead() || millis() - timer >= HttpSessionTimeout)
	{
		// Sometimes uploads can get stuck; make sure they are cancelled when that happens
		ConnectionLost();
		return;
	}

	// See if the upload has finished
	if (uploadedBytes >= postFileLength)
	{
		// Reset POST upload state for this client
		const IPAddress remoteIP = GetRemoteIP();
		for (size_t i = 0; i < numSessions; i++)
		{
			if (sessions[i].ip == remoteIP && sessions[i].isPostUploading)
			{
				sessions[i].isPostUploading = false;
				sessions[i].lastQueryTime = millis();
				break;
			}
		}

		FinishUpload(postFileLength, fileLastModified, postFileGotCrc, postFileExpectedCrc);
		SendJsonResponse("upload");
	}
}

#endif

// This is called to force termination if we implement the specified protocol
void HttpResponder::Terminate(NetworkProtocol protocol, NetworkInterface *interface) noexcept
{
	if (responderState != ResponderState::free && (protocol == HttpProtocol || protocol == AnyProtocol) && skt != nullptr && skt->GetInterface() == interface)
	{
		ConnectionLost();
	}
}

// This overrides the version in class NetworkResponder
void HttpResponder::CancelUpload() noexcept
{
	if (skt != nullptr)
	{
		for (size_t i = 0; i < numSessions; i++)
		{
			if (sessions[i].ip == skt->GetRemoteIP() && sessions[i].isPostUploading)
			{
				sessions[i].isPostUploading = false;
				sessions[i].lastQueryTime = millis();
				break;
			}
		}
	}
	UploadingNetworkResponder::CancelUpload();
}

// This overrides the version in class NetworkResponder
void HttpResponder::SendData() noexcept
{
	NetworkResponder::SendData();
	if (responderState == ResponderState::reading)
	{
		timer = millis();				// restart the timer
	}
}

void HttpResponder::Diagnostics(MessageType mt) const noexcept
{
	GetPlatform().MessageF(mt, " HTTP(%d)", (int)responderState);
}

/*static*/ void HttpResponder::InitStatic() noexcept
{
	gcodeReplyMutex.Create("HttpGCodeReply");
}

// This is called when we are shutting down the network or just this protocol. It may be called even if this protocol isn't enabled.
/*static*/ void HttpResponder::Disable() noexcept
{
	MutexLocker lock(gcodeReplyMutex);

	clientsServed = 0;
	numSessions = 0;
	gcodeReply.ReleaseAll();
}

// This is called from the GCodes task to store a response, which is picked up by the Network task
/*static*/ void HttpResponder::HandleGCodeReply(const char *_ecv_array reply) noexcept
{
	if (numSessions > 0)
	{
		MutexLocker lock(gcodeReplyMutex);

		OutputBuffer *buffer = gcodeReply.GetLastItem();
		if (buffer == nullptr || buffer->IsReferenced())
		{
			if (!OutputBuffer::Allocate(buffer))
			{
				// No more space available, stop here
				return;
			}
			if (!gcodeReply.Push(buffer))
			{
				// Can't push, so buffer was discarded. Don't append to it.
				return;
			}
		}

		buffer->cat(reply);
		clientsServed = 0;
		seq++;
	}
}

/*static*/ void HttpResponder::HandleGCodeReply(OutputBuffer *reply) noexcept
{
	if (reply != nullptr)
	{
		if (numSessions > 0)
		{
			// FIXME: This might cause G-code responses to be sent twice to fast HTTP clients, but
			// I (chrishamm) cannot think of a nicer way to deal with slow clients at the moment...
			MutexLocker lock(gcodeReplyMutex);

			gcodeReply.Push(reply);
			clientsServed = 0;
			seq++;
		}
		else
		{
			// Don't use buffers that may never get released...
			OutputBuffer::ReleaseAll(reply);
		}
	}
}

// Check for timed out sessions and old reply buffers
/*static*/ void HttpResponder::CheckSessions() noexcept
{
	unsigned int clientsTimedOut = 0;
	const uint32_t now = millis();
	for (size_t i = numSessions; i != 0; )
	{
		--i;
		if (now - sessions[i].lastQueryTime > HttpSessionTimeout)
		{
			RemoveSession(i);
			clientsTimedOut++;
		}
	}

	// If we cannot send the G-Code reply to anyone, we may free up some run-time space by dumping it
	if (clientsTimedOut != 0)
	{
		bool released = false;
		{
			MutexLocker lock(gcodeReplyMutex);

			clientsServed += clientsTimedOut;			// assume the disconnected clients haven't fetched the G-Code reply yet
			if (numSessions == 0 || clientsServed >= numSessions)
			{
				while (!gcodeReply.IsEmpty())
				{
					OutputBuffer *buf = gcodeReply.Pop();
					OutputBuffer::ReleaseAll(buf);
				}
				released = true;
			}
			clientsServed = 0;
		}
		if (released && reprap.Debug(moduleWebserver))
		{
			debugPrintf("Released gcodeReply, free buffers=%u\n", OutputBuffer::GetFreeBuffers());
		}
	}
	else if (!gcodeReply.IsEmpty())
	{
		// Check whether we can time out any GCode buffers
		bool released;
		{
			MutexLocker lock(gcodeReplyMutex);
			released = gcodeReply.ApplyTimeout(HttpSessionTimeout);
		}
		if (released && reprap.Debug(moduleWebserver))
		{
			debugPrintf("Timed out gcodeReply, free buffers=%u\n", OutputBuffer::GetFreeBuffers());
		}
	}
}

/*static*/ void HttpResponder::CommonDiagnostics(MessageType mtype) noexcept
{
	GetPlatform().MessageF(mtype, "HTTP sessions: %u of %u\n", numSessions, MaxHttpSessions);
}

void HttpResponder::AddCorsHeader() noexcept
{
	if (reprap.GetNetwork().GetCorsSite() != nullptr)
	{
		outBuf->catf("Access-Control-Allow-Origin: %s\r\n", reprap.GetNetwork().GetCorsSite());
	}
}

// Static data

HttpResponder::HttpSession HttpResponder::sessions[MaxHttpSessions];
unsigned int HttpResponder::numSessions = 0;
unsigned int HttpResponder::clientsServed = 0;

volatile uint16_t HttpResponder::seq = 0;
volatile OutputStack HttpResponder::gcodeReply;
Mutex HttpResponder::gcodeReplyMutex;

#endif // SUPPORT_HTTP

// End
