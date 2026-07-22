// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <type_traits>

#if !defined(kiss_fft_scalar)
#define kiss_fft_scalar double
#define MUSIC_PLAYER_LIBRARY_UNDEFINE_KISS_FFT_SCALAR
#endif

#include <kissfft/kiss_fft.h>

static_assert(std::is_same_v<kiss_fft_scalar, double>,
	"WpfMusicPlayer requires the double-precision KissFFT ABI");

#if defined(MUSIC_PLAYER_LIBRARY_UNDEFINE_KISS_FFT_SCALAR)
#undef MUSIC_PLAYER_LIBRARY_UNDEFINE_KISS_FFT_SCALAR
#undef kiss_fft_scalar
#endif

namespace MusicPlayerLibrary
{
	using KissFftConfigElement = std::remove_pointer_t<kiss_fft_cfg>;

	struct KissFftConfigDeleter final
	{
		void operator()(KissFftConfigElement* config) const noexcept
		{
			if (config != nullptr)
				kiss_fft_free(config);
		}
	};

	using UniqueKissFftConfig =
		std::unique_ptr<KissFftConfigElement, KissFftConfigDeleter>;

	[[nodiscard]] inline UniqueKissFftConfig AllocateKissFftConfig(
		const int fft_size,
		const bool inverse = false) noexcept
	{
		return UniqueKissFftConfig(
			kiss_fft_alloc(fft_size, inverse ? 1 : 0, nullptr, nullptr));
	}
}
