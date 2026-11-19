#include "rbcollectionreader.h"

#include "rapidxml/rapidxml.hpp"
#include "rapidxml/rapidxml_utils.hpp"

#include <utility/fileutils.h>
#include <nap/logger.h>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <regex>

#include "nap/timer.h"

RTTI_BEGIN_CLASS(nap::RbCollectionReader)
    RTTI_PROPERTY("CollectionFile", &nap::RbCollectionReader::mCollectionFile, nap::rtti::EPropertyMetaData::Required, "Path to rekordbox.xml file")
    RTTI_PROPERTY("DecodeURI", &nap::RbCollectionReader::mDecodeURI, nap::rtti::EPropertyMetaData::Default, "If true, decode URI encoded file paths in rekordbox.xml")
RTTI_END_CLASS

namespace nap
{
    namespace RBoxAttributes
    {
        // Track attributes
        constexpr const char* const TrackID     = "TrackID";
        constexpr const char* const Name         = "Name";
        constexpr const char* const Artist       = "Artist";
        constexpr const char* const Album        = "Album";
        constexpr const char* const Genre        = "Genre";
        constexpr const char* const Label        = "Label";
        constexpr const char* const Tonality     = "Tonality";
        constexpr const char* const FileName     = "FileName";
        constexpr const char* const Location     = "Location";
        constexpr const char* const DateAdded    = "DateAdded";
        constexpr const char* const TotalTime    = "TotalTime";
        constexpr const char* const Year         = "Year";
        constexpr const char* const PlayCount    = "PlayCount";
        constexpr const char* const Rating       = "Rating";
        constexpr const char* const AverageBpm   = "AverageBpm";
        constexpr const char* const Type         = "Type";
        constexpr const char* const TrackType    = "TrackType";
        constexpr const char* const Key          = "Key";

        // Position mark attributes
        constexpr const char* const PositionMark_Num   = "Num";
        constexpr const char* const PositionMark_Name  = "Name";
        constexpr const char* const PositionMark_Start = "Start";
    }

    namespace RBoxNodes
    {
        constexpr const char* const DJ_PLAYLISTS    = "DJ_PLAYLISTS";
        constexpr const char* const COLLECTION      = "COLLECTION";
        constexpr const char* const TRACK           = "TRACK";
        constexpr const char* const NODE            = "NODE";
        constexpr const char* const POSITION_MARK   = "POSITION_MARK";
        constexpr const char* const PLAYLISTS       = "PLAYLISTS";
    }

    std::string decodeURI(const std::string& encoded)
    {
        auto hex_value = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        // Decoded output is never longer than the input
        std::string decoded;
        decoded.reserve(encoded.size());

        const size_t size = encoded.size();
        for (size_t i = 0; i < size; ++i)
        {
            const char c = encoded[i];
            if (c == '%' && i + 2 < size)
            {
                const int hi = hex_value(encoded[i + 1]);
                const int lo = hex_value(encoded[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    decoded.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(c);
        }
        return decoded;
    }


    static std::string getAttribute(const rapidxml::xml_node<>* node, const char* name)
    {
        auto* attr = node->first_attribute(name);
        return attr != nullptr ? std::string(attr->value(), attr->value_size()) : std::string();
    }


    using TrackMap = std::unordered_map<int64_t, RbTrack>;
    using PlaylistMap = std::unordered_map<std::string, std::vector<const RbTrack*>>;

    /**
     * Recursively reads playlist nodes. Folders (Type 0) are traversed, playlists (Type 1) are stored by name.
     * Track keys that can't be found in the collection are skipped.
     */
    static void readPlaylistNodes(const rapidxml::xml_node<>* parent, const TrackMap& tracks, PlaylistMap& playlists)
    {
        for (auto* node = parent->first_node(RBoxNodes::NODE); node != nullptr; node = node->next_sibling(RBoxNodes::NODE))
        {
            if (getAttribute(node, RBoxAttributes::Type) == "0")
            {
                readPlaylistNodes(node, tracks, playlists);
                continue;
            }

            std::string name = getAttribute(node, RBoxAttributes::Name);
            if (playlists.find(name) != playlists.end())
                nap::Logger::warn("Duplicate playlist name '%s', merging entries", name.c_str());

            auto& playlist = playlists[name];
            for (auto* track_node = node->first_node(RBoxNodes::TRACK); track_node != nullptr; track_node = track_node->next_sibling(RBoxNodes::TRACK))
            {
                std::string key = getAttribute(track_node, RBoxAttributes::Key);
                char* end = nullptr;
                long long id = std::strtoll(key.c_str(), &end, 10);
                if (key.empty() || *end != '\0')
                {
                    nap::Logger::warn("Invalid track key '%s' in playlist '%s'", key.c_str(), name.c_str());
                    continue;
                }

                auto it = tracks.find(static_cast<int64_t>(id));
                if (it == tracks.end())
                {
                    nap::Logger::warn("Track %lld in playlist '%s' not found in collection", id, name.c_str());
                    continue;
                }
                playlist.emplace_back(&it->second);
            }
        }
    }


    bool RbCollectionReader::init(utility::ErrorState &errorState)
    {
        mTracks.clear();
        mPlaylists.clear();

        // Create timer to measure how long it takes to read the collection
        SteadyTimer timer; timer.start();

        // Read file into a null terminated, mutable buffer, as required by rapidxml
        std::vector<char> buffer;
        try
        {
            rapidxml::file<> xml_file(mCollectionFile.c_str());
            buffer.assign(xml_file.data(), xml_file.data() + xml_file.size());
            buffer.push_back('\0');
        }
        catch (const std::exception& e)
        {
            errorState.fail("Unable to read '%s': %s", mCollectionFile.c_str(), e.what());
            return false;
        }

        // Parse the XML document
        rapidxml::xml_document<> doc;
        try
        {
            doc.parse<0>(buffer.data());
        }
        catch (const rapidxml::parse_error& e)
        {
            errorState.fail("Unable to parse '%s': %s", mCollectionFile.c_str(), e.what());
            return false;
        }

        // Get the root node
        auto* root = doc.first_node(RBoxNodes::DJ_PLAYLISTS);
        if (!errorState.check(root != nullptr, "'%s' has no %s node",  mCollectionFile.c_str(), RBoxNodes::DJ_PLAYLISTS))
            return false;

        // Get the collection node
        auto* collection = root->first_node(RBoxNodes::COLLECTION);
        if (!errorState.check(collection != nullptr, "'%s' has no %s node", mCollectionFile.c_str(), RBoxNodes::COLLECTION))
            return false;

        // Tracks
        for (auto* track_node = collection->first_node(RBoxNodes::TRACK); track_node != nullptr; track_node = track_node->next_sibling(RBoxNodes::TRACK))
        {
            // Get the track ID attribute and convert it to a 64 bit integer
            auto* id_attr = track_node->first_attribute(RBoxAttributes::TrackID);
            if (!errorState.check(id_attr != nullptr, "%s entry without %s in '%s'", RBoxNodes::TRACK, RBoxAttributes::TrackID, mCollectionFile.c_str()))
                return false;

            // Convert id to a 64 bit integer
            std::string id_str(id_attr->value(), id_attr->value_size());
            errno = 0;
            char* end = nullptr;
            long long id = std::strtoll(id_str.c_str(), &end, 10);
            if (!errorState.check(errno == 0 && end != id_str.c_str() && *end == '\0', "Invalid TrackID '%s' in '%s'", id_str.c_str(), mCollectionFile.c_str()))
                return false;

            // Read track attributes
            RbTrack track;
            track.mTrackID   = static_cast<int64_t>(id);
            track.mName      = getAttribute(track_node, RBoxAttributes::Name);
            track.mArtist    = getAttribute(track_node, RBoxAttributes::Artist);
            track.mAlbum     = getAttribute(track_node, RBoxAttributes::Album);
            track.mGenre     = getAttribute(track_node, RBoxAttributes::Genre);
            track.mLabel     = getAttribute(track_node, RBoxAttributes::Label);
            track.mTonality  = getAttribute(track_node, RBoxAttributes::Tonality);
            track.mLocation  = getAttribute(track_node, RBoxAttributes::Location);
            track.mFilename  = decodeURI(utility::getFileName(track.mLocation));
            track.mDateAdded = getAttribute(track_node, RBoxAttributes::DateAdded);
            track.mTotalTime = std::atoi(getAttribute(track_node, RBoxAttributes::TotalTime).c_str());
            track.mYear      = std::atoi(getAttribute(track_node, RBoxAttributes::Year).c_str());
            track.mPlayCount = std::atoi(getAttribute(track_node, RBoxAttributes::PlayCount).c_str());
            track.mRating    = std::atoi(getAttribute(track_node, RBoxAttributes::Rating).c_str());
            track.mAverageBpm= std::atof(getAttribute(track_node, RBoxAttributes::AverageBpm).c_str());

            // Read position marks (hot cues)
            for (auto* pos_mark_node = track_node->first_node(RBoxNodes::POSITION_MARK); pos_mark_node != nullptr; pos_mark_node = pos_mark_node->next_sibling(RBoxNodes::POSITION_MARK))
            {
                RbTrack::PositionMark pos_mark;
                pos_mark.mNum   = std::atoi(getAttribute(pos_mark_node, RBoxAttributes::PositionMark_Num).c_str());
                pos_mark.mName  = getAttribute(pos_mark_node, RBoxAttributes::PositionMark_Name);
                pos_mark.mStart = std::atof(getAttribute(pos_mark_node, RBoxAttributes::PositionMark_Start).c_str());
                track.mPositionMarks.push_back(std::move(pos_mark));
            }

            // Store the track in the map, warn if the track ID is a duplicate and ignore the duplicate
            if (!mTracks.emplace(track.mTrackID, std::move(track)).second)
                nap::Logger::warn("Duplicate TrackID %lld in '%s', ignoring", id, mCollectionFile.c_str());
        }

        // Playlists
        mPlaylists.clear();
        if (auto* playlists_node = root->first_node(RBoxNodes::PLAYLISTS))
            readPlaylistNodes(playlists_node, mTracks, mPlaylists);

        // Done, log how long it took to read the collection and return success
        nap::Logger::info("Read %zu tracks and %zu playlists from '%s' in %f seconds", mTracks.size(), mPlaylists.size(), mCollectionFile.c_str(), timer.getElapsedTimeFloat());

        return true;
    }
}