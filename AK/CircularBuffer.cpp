/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CircularBuffer.h>
#include <AK/MemMem.h>
#include <AK/Stream.h>

namespace AK {

CircularBuffer::CircularBuffer(ByteBuffer buffer)
    : m_buffer(move(buffer))
{
}

ErrorOr<CircularBuffer> CircularBuffer::create_empty(size_t size)
{
    auto temporary_buffer = TRY(ByteBuffer::create_uninitialized(size));

    CircularBuffer circular_buffer { move(temporary_buffer) };

    return circular_buffer;
}

ErrorOr<CircularBuffer> CircularBuffer::create_initialized(ByteBuffer buffer)
{
    CircularBuffer circular_buffer { move(buffer) };

    circular_buffer.m_used_space = circular_buffer.m_buffer.size();

    return circular_buffer;
}

size_t CircularBuffer::empty_space() const
{
    return capacity() - m_used_space;
}

size_t CircularBuffer::used_space() const
{
    return m_used_space;
}

size_t CircularBuffer::capacity() const
{
    return m_buffer.size();
}

size_t CircularBuffer::seekback_limit() const
{
    return m_seekback_limit;
}

bool CircularBuffer::is_wrapping_around() const
{
    return capacity() <= m_reading_head + m_used_space;
}

Optional<size_t> CircularBuffer::offset_of(StringView needle, Optional<size_t> from, Optional<size_t> until) const
{
    auto const read_from = from.value_or(0);
    auto const read_until = until.value_or(m_used_space);
    VERIFY(read_from <= read_until);

    Array<ReadonlyBytes, 2> spans {};
    spans[0] = next_read_span();
    auto const original_span_0_size = spans[0].size();

    if (read_from > 0)
        spans[0] = spans[0].slice(min(spans[0].size(), read_from));

    if (spans[0].size() + read_from > read_until)
        spans[0] = spans[0].trim(read_until - read_from);
    else if (is_wrapping_around())
        spans[1] = m_buffer.span().slice(max(original_span_0_size, read_from) - original_span_0_size, min(read_until, m_used_space) - original_span_0_size);

    auto maybe_found = AK::memmem(spans.begin(), spans.end(), needle.bytes());
    if (maybe_found.has_value())
        *maybe_found += read_from;

    return maybe_found;
}

void CircularBuffer::clear()
{
    m_reading_head = 0;
    m_used_space = 0;
    m_seekback_limit = 0;
}

Bytes CircularBuffer::next_write_span()
{
    if (is_wrapping_around())
        return m_buffer.span().slice(m_reading_head + m_used_space - capacity(), capacity() - m_used_space);
    return m_buffer.span().slice(m_reading_head + m_used_space, capacity() - (m_reading_head + m_used_space));
}

ReadonlyBytes CircularBuffer::next_read_span() const
{
    return m_buffer.span().slice(m_reading_head, min(capacity() - m_reading_head, m_used_space));
}

ReadonlyBytes CircularBuffer::next_read_span_with_seekback(size_t distance) const
{
    VERIFY(m_seekback_limit <= capacity());
    VERIFY(distance <= m_seekback_limit);

    // Note: We are adding the capacity once here to ensure that we can wrap around the negative space by using modulo.
    auto read_offset = (capacity() + m_reading_head + m_used_space - distance) % capacity();

    return m_buffer.span().slice(read_offset, min(capacity() - read_offset, distance));
}

size_t CircularBuffer::write(ReadonlyBytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_write_span();
        if (next_span.size() == 0)
            break;

        auto const written_bytes = bytes.slice(bytes.size() - remaining).copy_trimmed_to(next_span);

        m_used_space += written_bytes;

        m_seekback_limit += written_bytes;
        if (m_seekback_limit > capacity())
            m_seekback_limit = capacity();

        remaining -= written_bytes;
    }

    return bytes.size() - remaining;
}

Bytes CircularBuffer::read(Bytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_read_span();
        if (next_span.size() == 0)
            break;

        auto written_bytes = next_span.copy_trimmed_to(bytes.slice(bytes.size() - remaining));

        m_used_space -= written_bytes;
        m_reading_head += written_bytes;

        if (m_reading_head >= capacity())
            m_reading_head -= capacity();

        remaining -= written_bytes;
    }

    return bytes.trim(bytes.size() - remaining);
}

ErrorOr<Bytes> CircularBuffer::read_with_seekback(Bytes bytes, size_t distance)
{
    if (distance > m_seekback_limit)
        return Error::from_string_literal("Tried a seekback read beyond the seekback limit");

    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_read_span_with_seekback(distance);
        if (next_span.size() == 0)
            break;

        auto written_bytes = next_span.copy_trimmed_to(bytes.slice(bytes.size() - remaining));

        distance -= written_bytes;
        remaining -= written_bytes;
    }

    return bytes.trim(bytes.size() - remaining);
}

ErrorOr<void> CircularBuffer::discard(size_t discarding_size)
{
    if (m_used_space < discarding_size)
        return Error::from_string_literal("Can not discard more data than what the buffer contains");
    m_used_space -= discarding_size;
    m_reading_head = (m_reading_head + discarding_size) % capacity();

    return {};
}

ErrorOr<size_t> CircularBuffer::fill_from_stream(Stream& stream)
{
    auto next_span = next_write_span();
    if (next_span.size() == 0)
        return 0;

    auto bytes = TRY(stream.read_some(next_span));
    m_used_space += bytes.size();

    m_seekback_limit += bytes.size();
    if (m_seekback_limit > capacity())
        m_seekback_limit = capacity();

    return bytes.size();
}

ErrorOr<size_t> CircularBuffer::flush_to_stream(Stream& stream)
{
    auto next_span = next_read_span();
    if (next_span.size() == 0)
        return 0;

    auto written_bytes = TRY(stream.write_some(next_span));

    m_used_space -= written_bytes;
    m_reading_head += written_bytes;

    if (m_reading_head >= capacity())
        m_reading_head -= capacity();

    return written_bytes;
}

ErrorOr<size_t> CircularBuffer::copy_from_seekback(size_t distance, size_t length)
{
    if (distance > m_seekback_limit)
        return Error::from_string_literal("Tried a seekback copy beyond the seekback limit");

    auto remaining_length = length;
    while (remaining_length > 0) {
        if (empty_space() == 0)
            break;

        auto next_span = next_read_span_with_seekback(distance);
        if (next_span.size() == 0)
            break;

        auto length_written = write(next_span.trim(remaining_length));
        remaining_length -= length_written;

        // If we copied right from the end of the seekback area (i.e. our length is larger than the distance)
        // and the last copy was one complete "chunk", we can now double the distance to copy twice as much data in one go.
        if (remaining_length > distance && length_written == distance)
            distance *= 2;
    }

    return length - remaining_length;
}

ErrorOr<Vector<CircularBuffer::Match>> CircularBuffer::find_copy_in_seekback(size_t maximum_length, size_t minimum_length, Optional<Vector<size_t> const&> distance_hints) const
{
    VERIFY(minimum_length > 0);

    // Clip the maximum length to the amount of data that we actually store.
    if (maximum_length > m_used_space)
        maximum_length = m_used_space;

    if (maximum_length < minimum_length)
        return Vector<Match> {};

    Vector<Match> matches;

    if (distance_hints.has_value()) {
        // If we have any hints, verify and use those.
        for (auto const& distance : distance_hints.value()) {
            // TODO: This does not yet support looping repetitions.
            if (distance < minimum_length)
                continue;

            auto needle_offset = (capacity() + m_reading_head) % capacity();
            auto haystack_offset = (capacity() + m_reading_head - distance) % capacity();

            for (size_t i = 0; i < minimum_length; i++) {
                if (m_buffer[needle_offset] != m_buffer[haystack_offset])
                    break;

                needle_offset = (needle_offset + 1) % capacity();
                haystack_offset = (haystack_offset + 1) % capacity();

                if (i + 1 == minimum_length)
                    TRY(matches.try_empend(distance, minimum_length));
            }
        }
    } else {
        // Otherwise, use memmem to find the initial matches.
        // Note: We have the read head as our reference point, but `next_read_span_with_seekback` isn't aware of that and continues to use the write head.
        //       Therefore, we need to make sure to slice off the extraneous bytes from the end of the span and shift the returned distances by the correct amount.
        size_t haystack_offset_from_start = 0;
        Vector<ReadonlyBytes, 2> haystack;
        haystack.append(next_read_span_with_seekback(m_seekback_limit));
        if (haystack[0].size() < m_seekback_limit - used_space())
            haystack.append(next_read_span_with_seekback(m_seekback_limit - haystack[0].size()));

        haystack.last() = haystack.last().trim(haystack.last().size() - used_space());

        auto needle = next_read_span().trim(minimum_length);

        auto memmem_match = AK::memmem(haystack.begin(), haystack.end(), needle);
        while (memmem_match.has_value()) {
            auto match_offset = memmem_match.release_value();

            // Add the match to the list of matches to work with.
            TRY(matches.try_empend(m_seekback_limit - used_space() - haystack_offset_from_start - match_offset, minimum_length));

            auto size_to_discard = match_offset + 1;

            // Trim away the already processed bytes from the haystack.
            haystack_offset_from_start += size_to_discard;
            while (size_to_discard > 0) {
                if (haystack[0].size() < size_to_discard) {
                    size_to_discard -= haystack[0].size();
                    haystack.remove(0);
                } else {
                    haystack[0] = haystack[0].slice(size_to_discard);
                    break;
                }
            }

            if (haystack.size() == 0)
                break;

            // Try and find the next match.
            memmem_match = AK::memmem(haystack.begin(), haystack.end(), needle);
        }
    }

    // From now on, all matches that we have stored have at least a length of `minimum_length` and they all refer to the same value.
    // For the remaining part, we will keep checking the next byte incrementally and keep eliminating matches until we eliminated all of them.
    Vector<Match> next_matches;

    for (size_t offset = minimum_length; offset < maximum_length; offset++) {
        auto needle_data = m_buffer[(capacity() + m_reading_head + offset) % capacity()];

        for (auto const& match : matches) {
            auto haystack_data = m_buffer[(capacity() + m_reading_head - match.distance + offset) % capacity()];

            if (haystack_data != needle_data)
                continue;

            TRY(next_matches.try_empend(match.distance, match.length + 1));
        }

        if (next_matches.size() == 0)
            return matches;

        swap(matches, next_matches);
        next_matches.clear_with_capacity();
    }

    return matches;
}

}
