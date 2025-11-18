#include "pch.h"
#include "libaccess_winrt_plugin.h"

#include <vlc_access.h>
#include <vlc_input.h>
#include <vlc_charset.h>


using namespace winrt;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Streams;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Foundation;


/*****************************************************************************
* Module descriptor
*****************************************************************************/

vlc_module_begin()
set_shortname(N_("WinRTInput"))
set_description(N_("WinRT input"))
set_category(CAT_INPUT)
set_subcategory(SUBCAT_INPUT_ACCESS)
set_capability("access", 80)
add_shortcut("winrt", "file")
set_callbacks(&Open, &Close)
vlc_module_end()


struct access_sys_t
{
	IRandomAccessStream read_stream;
	DataReader			data_reader;
	uint64_t            i_pos;
	uint32_t			retries;
	bool                b_eof;
};

namespace
{
	/* Check if token is a valid GUID */
	bool is_shared_access_token_valid(const hstring& access_token) {
		// GUID have fixed length
		if (access_token.size() != 36)
			return false;

		for (uint32_t i = 0; i < access_token.size(); ++i)
		{
			const auto wc = access_token[i];

			if (i == 8 || i == 13 || i == 18 || i == 23)
			{
				// Check hyphens are at the correct positions (8 13 18 23)
				if (wc != '-')
					return false;
			}
			else
			{
				// Other characters must be valid hex
				if (!iswxdigit(wc))
					return false;
			}
		}

		return true;
	}

	bool is_future_access_token_valid(const hstring& access_token) {
		// GUID have fixed length
		if (access_token.size() != 38 || access_token[0] != '{' || access_token[37] != '}')
			return false;

		for (uint32_t i = 1; i < access_token.size() - 1; ++i)
		{
			const auto wc = access_token[i];

			if (i == 9 || i == 14 || i == 19 || i == 24)
			{
				// Check hyphens are at the correct positions (9 14 19 24)
				if (wc != '-')
					return false;
			}
			else
			{
				// Other characters must be valid hex
				if (!iswxdigit(wc))
					return false;
			}
		}

		return true;
	}

	void set_stream(access_sys_t* p_sys, const IRandomAccessStream& stream)
	{
		auto reader = DataReader(stream);
		reader.InputStreamOptions(InputStreamOptions::Partial | InputStreamOptions::ReadAhead);
		p_sys->read_stream = stream;
		p_sys->data_reader = reader;
	}

	IAsyncAction open_file_from_path_async(access_sys_t* p_sys, const hstring& path)
	{
		auto file = co_await StorageFile::GetFileFromPathAsync(path);
		auto stream = co_await file.OpenReadAsync();
		set_stream(p_sys, stream);
	}

	int open_file_from_path(access_sys_t* p_sys, const hstring& path)
	{
		try
		{
			open_file_from_path_async(p_sys, path).get();
			return VLC_SUCCESS;
		}
		catch (hresult_error const& ex)
		{
			OutputDebugString(ex.message().c_str());
			OutputDebugString(L"Failed to open file.");
			return VLC_EGENERIC;
		}
	}

	IAsyncOperation<IRandomAccessStreamWithContentType> open_file_from_future_access_token_async(const hstring& token)
	{
		auto file = co_await StorageApplicationPermissions::FutureAccessList().GetFileAsync(token);
		auto stream = co_await file.OpenReadAsync();
		co_return stream;
	}

	int open_file_from_future_access_token(access_sys_t* p_sys, const hstring& token)
	{
		try
		{
			auto stream = open_file_from_future_access_token_async(token).get();
			set_stream(p_sys, stream);
			return VLC_SUCCESS;
		}
		catch (hresult_error const& ex)
		{
			OutputDebugString(ex.message().c_str());
			OutputDebugString(L"Failed to open file.");
			return VLC_EGENERIC;
		}
	}

	IAsyncOperation<IRandomAccessStreamWithContentType> open_file_from_shared_access_token_async(const hstring& token)
	{
		auto file = co_await SharedStorageAccessManager::RedeemTokenForFileAsync(token);
		auto stream = co_await file.OpenReadAsync();
		co_return stream;
	}

	int open_file_from_shared_access_token(access_sys_t* p_sys, const hstring& token)
	{
		try
		{
			auto stream = open_file_from_shared_access_token_async(token).get();
			set_stream(p_sys, stream);
			return VLC_SUCCESS;
		}
		catch (hresult_error const& ex)
		{
			OutputDebugString(ex.message().c_str());
			OutputDebugString(L"Failed to open file.");
			return VLC_EGENERIC;
		}
	}

	int noop_open_fn(access_sys_t*, const hstring&) { return VLC_EGENERIC; }

	auto get_open_function(const hstring& token)
	{
		if (is_shared_access_token_valid(token))
			return open_file_from_shared_access_token;

		if (is_future_access_token_valid(token))
			return open_file_from_future_access_token;

		return noop_open_fn;
	}

	IAsyncOperation<unsigned int> read_async(const DataReader& reader, array_view<uint8_t> buffer)
	{
		const auto bytes_loaded = co_await reader.LoadAsync(buffer.size());
		buffer = array_view(buffer.data(), bytes_loaded);
		reader.ReadBytes(buffer);
		co_return bytes_loaded;
	}

	/* */
	int seek(stream_t* access, uint64_t position)
	{
		access_sys_t* p_sys = static_cast<access_sys_t*>(access->p_sys);

		try
		{
			auto clone_stream = p_sys->read_stream.CloneStream();
			clone_stream.Seek(position);
			set_stream(p_sys, clone_stream);
			p_sys->i_pos = position;
			p_sys->b_eof = p_sys->read_stream.Position() >= p_sys->read_stream.Size();
		}
		catch (hresult_error const& ex)
		{
			OutputDebugString(ex.message().c_str());
			return VLC_EGENERIC;
		}

		return VLC_SUCCESS;
	}

	/* */
	int control(stream_t* access, int query, va_list args)
	{
		const auto p_sys = static_cast<access_sys_t*>(access->p_sys);

		VLC_UNUSED(access);
		switch (query)
		{
		case STREAM_CAN_FASTSEEK:
		case STREAM_CAN_PAUSE:
		case STREAM_CAN_SEEK:
		case STREAM_CAN_CONTROL_PACE: {
			bool* b = va_arg(args, bool*);
			*b = true;
			return VLC_SUCCESS;
		}

		case STREAM_GET_PTS_DELAY: {
			int64_t* d = va_arg(args, int64_t*);
			*d = DEFAULT_PTS_DELAY;
			return VLC_SUCCESS;
		}

		case STREAM_SET_PAUSE_STATE:
			return VLC_SUCCESS;

		case STREAM_GET_SIZE: {
			*va_arg(args, uint64_t*) = p_sys->read_stream.Size();
			return VLC_SUCCESS;
		}
		default:
			return VLC_EGENERIC;
		}
	}

	/* */
	ssize_t read(stream_t* access, void* buffer, size_t size)
	{
		if (buffer == nullptr)
		{
			if (seek(access, size) == VLC_SUCCESS)
				return size;
			return 0;
		}

		access_sys_t* p_sys = static_cast<access_sys_t*>(access->p_sys);

		unsigned int total_read;
		const auto buffer_view = array_view(static_cast<uint8_t*>(buffer), static_cast<uint32_t>(size));

		try
		{
			total_read = read_async(p_sys->data_reader, buffer_view).get(); /* block with wait since we're in a worker thread */
		}
		catch (hresult_error const& ex)
		{
			OutputDebugString(L"Failure while reading block\n");
			if (ex.code() == HRESULT_FROM_WIN32(ERROR_OPLOCK_HANDLE_CLOSED)) {
				if (open_file_from_path(p_sys, to_hstring(access->psz_location)) == VLC_SUCCESS
					&& seek(access, p_sys->i_pos) == VLC_SUCCESS
					&& p_sys->retries < 3) {
					p_sys->retries++;
					return read(access, buffer, size);
				}
				OutputDebugString(L"Failed to reopen file\n");
			}
			return 0;
		}

		p_sys->i_pos += total_read;
		p_sys->b_eof = p_sys->read_stream.Position() >= p_sys->read_stream.Size();
		p_sys->retries = 0;
		if (p_sys->b_eof) {
			OutputDebugString(L"End of file reached\n");
		}

		return total_read;
	}
}

int Open(vlc_object_t* object)
{
	stream_t* access = reinterpret_cast<stream_t*>(object);
	hstring access_token;
	int (*pf_open)(access_sys_t*, const hstring&);

	if (strncmp(access->psz_name, "winrt", 5) == 0) {
		access_token = to_hstring(access->psz_location);
		pf_open = get_open_function(access_token);
	}
	else if (strncmp(access->psz_name, "file", 4) == 0) {
		char* pos = strstr(access->psz_filepath, "winrt:\\\\");
		if (pos && strlen(pos) > 8) {
			access_token = to_hstring(pos + 8);
			pf_open = get_open_function(access_token);
		}
		else
		{
			pf_open = open_file_from_path;
			access_token = to_hstring(access->psz_filepath);
		}
	}
	else
		return VLC_EGENERIC;

	if (pf_open == noop_open_fn)
		return VLC_EGENERIC;

	const auto p_sys = new(std::nothrow) access_sys_t{nullptr, nullptr, 0, false};
	if (p_sys == nullptr)
		return VLC_EGENERIC;

	access->p_sys = p_sys;
	if (pf_open(p_sys, access_token) != VLC_SUCCESS) {
		OutputDebugStringW(L"Error opening file with Path");
		Close(object);
		return VLC_EGENERIC;
	}

	access->pf_read = &read;
	access->pf_seek = &seek;
	access->pf_control = &control;

	return VLC_SUCCESS;
}

void Close(vlc_object_t* object)
{
	stream_t* access = reinterpret_cast<stream_t*>(object);
	access_sys_t* p_sys = static_cast<access_sys_t*>(access->p_sys);
	if (p_sys->data_reader != nullptr) {
		p_sys->data_reader = nullptr;
	}
	if (p_sys->read_stream != nullptr) {
		p_sys->read_stream = nullptr;
	}
	delete p_sys;
}

