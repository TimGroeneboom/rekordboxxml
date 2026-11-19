#pragma once

#include <nap/resource.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

namespace nap
{
    /**
     * A single track entry in the rekordbox collection
     */
    struct RbTrack
    {
        struct PositionMark
        {
            int mNum = 0;
            std::string mName = "";
            float mStart = 0.0f;
            std::string mType = "";
        };

        int64_t mTrackID = 0;
        std::string mName;
        std::string mArtist;
        std::string mAlbum;
        std::string mGenre;
        std::string mLabel;
        std::string mTonality;
        std::string mFilename;
        std::string mLocation;
        std::string mDateAdded;
        int mTotalTime = 0;     //!< Duration in seconds
        int mYear = 0;
        int mPlayCount = 0;
        int mRating = 0;
        double mAverageBpm = 0.0;
        std::vector<PositionMark> mPositionMarks;
    };

    class NAPAPI RbCollectionReader : public Resource
    {
        RTTI_ENABLE(Resource)
    public:
        bool init(utility::ErrorState &errorState) override;

        // Properties
        std::string mCollectionFile = "recordbox.xml"; //!< Path to rekordbox.xml file
        bool mDecodeURI = true; //!< If true, decode URI encoded file paths in rekordbox.xml

        /**
         * @return all collection tracks, keyed by track id
         */
        const std::unordered_map<int64_t, RbTrack>& getTracks() const { return mTracks; }

        /**
         * @return all playlists, keyed by playlist name
         */
        const std::unordered_map<std::string, std::vector<const RbTrack*>>& getPlaylists() const { return mPlaylists; }

    private:
        std::unordered_map<int64_t, RbTrack> mTracks;
        std::unordered_map<std::string, std::vector<const RbTrack*>> mPlaylists;
    };
}
