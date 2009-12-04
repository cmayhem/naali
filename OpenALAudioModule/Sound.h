// For conditions of distribution and use, see copyright notice in license.txt
#ifndef incl_OpenALAudio_Sound_h
#define incl_OpenALAudio_Sound_h

#include <al.h>

namespace OpenALAudio
{
    //! A sound buffer containing sound data. Uses OpenAL
    class Sound
    {
    public:
        //! OpenAL handle
        ALuint handle_;
    };
}

#endif