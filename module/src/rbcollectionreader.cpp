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
        for (auto* node = parent->first_node("NODE"); node != nullptr; node = node->next_sibling("NODE"))
        {
            if (getAttribute(node, "Type") == "0")
            {
                readPlaylistNodes(node, tracks, playlists);
                continue;
            }

            std::string name = getAttribute(node, "Name");
            if (playlists.find(name) != playlists.end())
                nap::Logger::warn("Duplicate playlist name '%s', merging entries", name.c_str());

            auto& playlist = playlists[name];
            for (auto* track_node = node->first_node("TRACK"); track_node != nullptr; track_node = track_node->next_sibling("TRACK"))
            {
                std::string key = getAttribute(track_node, "Key");
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
            return errorState.check(false, "Unable to read '%s': %s", mCollectionFile.c_str(), e.what());
        }

        rapidxml::xml_document<> doc;
        try
        {
            doc.parse<0>(buffer.data());
        }
        catch (const rapidxml::parse_error& e)
        {
            return errorState.check(false, "Unable to parse '%s': %s", mCollectionFile.c_str(), e.what());
        }

        auto* root = doc.first_node("DJ_PLAYLISTS");
        if (!errorState.check(root != nullptr, "'%s' has no DJ_PLAYLISTS node", mCollectionFile.c_str()))
            return false;

        auto* collection = root->first_node("COLLECTION");
        if (!errorState.check(collection != nullptr, "'%s' has no COLLECTION node", mCollectionFile.c_str()))
            return false;

        for (auto* track_node = collection->first_node("TRACK"); track_node != nullptr; track_node = track_node->next_sibling("TRACK"))
        {
            auto* id_attr = track_node->first_attribute("TrackID");
            if (!errorState.check(id_attr != nullptr, "TRACK entry without TrackID in '%s'", mCollectionFile.c_str()))
                return false;

            // Convert id to a 64 bit integer
            std::string id_str(id_attr->value(), id_attr->value_size());
            errno = 0;
            char* end = nullptr;
            long long id = std::strtoll(id_str.c_str(), &end, 10);
            if (!errorState.check(errno == 0 && end != id_str.c_str() && *end == '\0', "Invalid TrackID '%s' in '%s'", id_str.c_str(), mCollectionFile.c_str()))
                return false;

            RbTrack track;
            track.mTrackID   = static_cast<int64_t>(id);
            track.mName      = getAttribute(track_node, "Name");
            track.mArtist    = getAttribute(track_node, "Artist");
            track.mAlbum     = getAttribute(track_node, "Album");
            track.mGenre     = getAttribute(track_node, "Genre");
            track.mLabel     = getAttribute(track_node, "Label");
            track.mTonality  = getAttribute(track_node, "Tonality");
            track.mFilepath  = mDecodeURI ? decodeURI(getAttribute(track_node, "Location")) : getAttribute(track_node, "Location");
            track.mFilename  = utility::getFileName(track.mFilepath);
            track.mDateAdded = getAttribute(track_node, "DateAdded");
            track.mTotalTime = std::atoi(getAttribute(track_node, "TotalTime").c_str());
            track.mYear      = std::atoi(getAttribute(track_node, "Year").c_str());
            track.mPlayCount = std::atoi(getAttribute(track_node, "PlayCount").c_str());
            track.mRating    = std::atoi(getAttribute(track_node, "Rating").c_str());
            track.mAverageBpm= std::atof(getAttribute(track_node, "AverageBpm").c_str());

            if (!mTracks.emplace(track.mTrackID, std::move(track)).second)
                nap::Logger::warn("Duplicate TrackID %lld in '%s', ignoring", id, mCollectionFile.c_str());
        }

        // Playlists
        mPlaylists.clear();
        if (auto* playlists_node = root->first_node("PLAYLISTS"))
            readPlaylistNodes(playlists_node, mTracks, mPlaylists);

        nap::Logger::info("Read %zu tracks and %zu playlists from '%s' in %f seconds", mTracks.size(), mPlaylists.size(), mCollectionFile.c_str(), timer.getElapsedTimeFloat());
        return true;
    }
}