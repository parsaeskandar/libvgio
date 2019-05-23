/**
 * \file message_iterator.cpp
 * Implementations for the MessageIterator for reading type-tagged grouped message files
 */

#include "vg/io/message_iterator.hpp"
#include "vg/io/registry.hpp"

namespace vg {

namespace io {

using namespace std;

// Provide the static values a compilation unit to live in.
const size_t MessageIterator::MAX_MESSAGE_SIZE;

MessageIterator::MessageIterator(istream& in) : MessageIterator(unique_ptr<BlockedGzipInputStream>(new BlockedGzipInputStream(in))) {
    // Nothing to do!
}

MessageIterator::MessageIterator(unique_ptr<BlockedGzipInputStream>&& bgzf) :
    value(),
    previous_tag(),
    group_count(0),
    group_idx(0),
    group_vo(-1),
    item_vo(-1),
    bgzip_in(std::move(bgzf))
{
    advance();
}

auto MessageIterator::operator*() const -> const TaggedMessage& {
    return value;
}

auto MessageIterator::operator*() -> TaggedMessage& {
    return value;
}


auto MessageIterator::operator++() -> const MessageIterator& {
    while (group_count == group_idx) {
        // We have made it to the end of the group we are reading. We will
        // start a new group now (and skip through empty groups).
        
        // Determine exactly where we are positioned, if possible, before
        // creating the CodedInputStream to read the group's item count
        auto virtual_offset = bgzip_in->Tell();
        
        if (virtual_offset == -1) {
            // We don't have seek capability, so we just count up the groups we read.
            // On construction this is -1; bump it up to 0 for the first group.
            group_vo++;
        } else {
            // We can seek. We need to know what offset we are at
            group_vo = virtual_offset;
        }
        
        // Start at the start of the new group
        group_idx = 0;
        
        // Make a CodedInputStream to read the group length
        ::google::protobuf::io::CodedInputStream coded_in(bgzip_in.get());
        // Alot space for group's length, tag's length, and tag (generously)
        coded_in.SetTotalBytesLimit(MAX_MESSAGE_SIZE * 2, MAX_MESSAGE_SIZE * 2);
        
        // Try and read the group's length
        if (!coded_in.ReadVarint64((::google::protobuf::uint64*) &group_count)) {
            // We didn't get a length
            
#ifdef debug
            cerr << "Failed to read group count at " << group_vo << "; stop iteration." << endl;
#endif
            
            // This is the end of the input stream, switch to state that
            // will match the end constructor
            group_vo = -1;
            item_vo = -1;
            value.first.clear();
            value.second.reset();
            return *this;
        }
        
#ifdef debug
        cerr << "Read group count at " << group_vo << ": " << group_count << endl;
#endif
        
        // Now we have to grab the tag, which is pretending to be the first item.
        // It could also be the first item, if it isn't a known tag string.
        
        // Get the tag's virtual offset, if available
        virtual_offset = bgzip_in->Tell();
        
        // The tag is prefixed by its size
        uint32_t tagSize = 0;
        handle(coded_in.ReadVarint32(&tagSize));
        
        if (tagSize > MAX_MESSAGE_SIZE) {
            throw runtime_error("[io::MessageIterator::advance] tag of " +
                                to_string(tagSize) + " bytes is too long");
        }
        
        // Read it into the tag field of our value
        value.first.clear();
        if (tagSize) {
            handle(coded_in.ReadString(&value.first, tagSize));
        }
        
#ifdef debug
        cerr << "Read what should be the tag of " << tagSize << " bytes" << endl;
#endif
        
        // Update the counters for the tag, which the counters treat as a message.
        if (virtual_offset == -1) {
            // Just track the counter.
            item_vo++;
        } else {
            // We know where here is
            item_vo = virtual_offset;
        }
        
        // Move on to the next message in the group
        group_idx++;
    
        // Work out if this really is a tag.
        bool is_tag = false;
        
        if (!previous_tag.empty() && previous_tag == value.first) {
#ifdef debug
            cerr << "Tag is the same as the last tag of \"" << previous_tag << "\"" << endl;
#endif
            is_tag = true;
        } else {
#ifdef debug
            cerr << "Tag does not match cached previous tag or there is no cached previous tag" << endl;
#endif
        }
    
        if (!is_tag && Registry::is_valid_tag(value.first)) {
#ifdef debug
            cerr << "Tag \"" << value.first << "\" is OK with the registry" << endl;
#endif
            is_tag = true;
        } else if (!is_tag) {
#ifdef debug
            cerr << "Tag is not approved by the registry" << endl;
#endif
        }
    
        if (!is_tag) {
            // If we get here, the registry doesn't think it's a tag.
            // Assume it is actually a message, and make the group's tag ""
            value.second = make_unique<string>(std::move(value.first));
            value.first.clear();
            previous_tag.clear();
            
#ifdef debug
            cerr << "Tag is actually a message probably." << endl;
#endif

#ifdef debug
            cerr << "Found message with tag \"" << value.first << "\"" << endl;
#endif
            
            // Return ourselves, after increment
            return *this;
        }
        
        // Otherwise this is a real tag.
        // Back up its value in case our pair gets moved away.
        previous_tag = value.first;
        
        if (is_tag && group_count == 1) {
            // This group is a tag *only*.
            // If we hit the end of the loop we'll just skip over it.
            // We want to emit it as a pair of (tag, null).
            // So we consider our increment complete here.
            
#ifdef debug
            cerr << "Found message-less tag \"" << value.first << "\"" << endl;
#endif
            
            value.second.reset();
            return *this;
        }
        
        // We continue through all empty groups.
    }
    
    // Now we know we have a message to go with our tag.
    
    // Now we know we're in a group, and we know the tag, if any.
    
    // Get the item's virtual offset, if available
    auto virtual_offset = bgzip_in->Tell();
    
    // We need a fresh CodedInputStream every time, because of the total byte limit
    ::google::protobuf::io::CodedInputStream coded_in(bgzip_in.get());
    // Alot space for size and item (generously)
    coded_in.SetTotalBytesLimit(MAX_MESSAGE_SIZE * 2, MAX_MESSAGE_SIZE * 2);
    
    // A message starts here
    if (virtual_offset == -1) {
        // Just track the counter.
        item_vo++;
    } else {
        // We know where here is
        item_vo = virtual_offset;
    }
    
    // The messages are prefixed by their size
    uint32_t msgSize = 0;
    handle(coded_in.ReadVarint32(&msgSize));
    
    if (msgSize > MAX_MESSAGE_SIZE) {
        throw runtime_error("[io::MessageIterator::advance] message of " +
                            to_string(msgSize) + " bytes is too long");
    }
    
    
    // We have a message.
    // Make an empty string to hold it.
    if (value.second.get() != nullptr) {
        value.second->clear();
    } else {
        value.second = make_unique<string>();
    }
    if (msgSize) {
        handle(coded_in.ReadString(value.second.get(), msgSize));
    }
    
    // Fill in the tag from the previous to make sure our value pair actually has it.
    // It may have been moved away.
    value.first = previous_tag;
    
#ifdef debug
    cerr << "Found message " << group_idx << " size " << msgSize << " with tag \"" << value.first << "\"" << endl;
#endif
    
    // Move on to the next message in the group
    group_idx++;
    
    // Return ourselves, after increment
    return *this;
}

auto MessageIterator::operator==(const MessageIterator& other) const -> bool {
    // Just ask if we both agree on whether we hit the end.
    return has_current() == other.has_current();
}
    
auto MessageIterator::operator!=(const MessageIterator& other) const -> bool {
    // Just ask if we disagree on whether we hit the end.
    return has_current() != other.has_current();
}

auto MessageIterator::has_current() const -> bool {
    return item_vo != -1;
}

auto MessageIterator::advance() -> void {
    // Run increment but don't return anything.
    ++(*this);
}

auto MessageIterator::take() -> TaggedMessage {
    auto temp = std::move(value);
    advance();
    // Return by value, which gets moved.
    return temp;
}

auto MessageIterator::tell_group() const -> int64_t {
    if (bgzip_in->Tell() != -1) {
        // The backing file supports seek/tell (which we ascertain by attempting it).
        if (group_vo == -1) {
            // We hit EOF and have no loaded message
            return bgzip_in->Tell();
        } else {
            // Return the *group's* virtual offset (not the current one)
            return group_vo;
        }
    } else {
        // group_vo holds a count. But we need to say we can't seek.
        return -1;
    }
}

auto MessageIterator::seek_group(int64_t virtual_offset) -> bool {
    if (virtual_offset < 0) {
        // That's not allowed
#ifdef debug
        cerr << "Can't seek to negative position" << endl;
#endif
        return false;
    }
    
    if (group_idx == 0 && group_vo == virtual_offset) {
        // We are there already
#ifdef debug
        cerr << "Already at seek position" << endl;
#endif
        return true;
    }
    
    // Try and do the seek
    bool sought = bgzip_in->Seek(virtual_offset);
    
    if (!sought) {
        // We can't seek
#ifdef debug
        cerr << "bgzip_in could not seek" << endl;
#endif
        return false;
    }
    
    // Get ready to read the group that's here
    group_count = 0;
    group_idx = 0;
    
#ifdef debug
    cerr << "Successfully sought" << endl;
#endif
    
    // Read it (or detect EOF)
    advance();
    
    // It worked!
    return true;
}

auto MessageIterator::range(istream& in) -> pair<MessageIterator, MessageIterator> {
    return make_pair(MessageIterator(in), MessageIterator());
}

auto MessageIterator::handle(bool ok) -> void {
    if (!ok) {
        throw runtime_error("[io::MessageIterator] obsolete, invalid, or corrupt protobuf input");
    }
}

}

}
