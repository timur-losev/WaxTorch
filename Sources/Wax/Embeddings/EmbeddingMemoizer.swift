import Foundation
import WaxVectorSearch

actor EmbeddingMemoizer {
    private struct Entry {
        var key: UInt64
        var value: [Float]
        var prev: UInt64?
        var next: UInt64?
    }

    private let capacity: Int
    private var entries: [UInt64: Entry] = [:]
    private var head: UInt64?
    private var tail: UInt64?

    init(capacity: Int) {
        self.capacity = max(0, capacity)
    }

    func get(_ key: UInt64) -> [Float]? {
        guard capacity > 0 else { return nil }
        guard var entry = entries[key] else { return nil }
        moveToFront(&entry)
        return entry.value
    }

    func set(_ key: UInt64, value: [Float]) {
        guard capacity > 0 else { return }
        if var existing = entries[key] {
            existing.value = value
            moveToFront(&existing)
            return
        }

        var entry = Entry(key: key, value: value, prev: nil, next: head)
        if let headKey = head, var currentHead = entries[headKey] {
            currentHead.prev = key
            entries[headKey] = currentHead
        } else {
            tail = key
        }
        head = key
        entries[key] = entry

        if entries.count > capacity, let tailKey = tail {
            remove(tailKey)
        }
    }

    private func moveToFront(_ entry: inout Entry) {
        let key = entry.key
        if head == key {
            entries[key] = entry
            return
        }

        let prevKey = entry.prev
        let nextKey = entry.next

        if let prevKey, var prev = entries[prevKey] {
            prev.next = nextKey
            entries[prevKey] = prev
        }
        if let nextKey, var next = entries[nextKey] {
            next.prev = prevKey
            entries[nextKey] = next
        }
        if tail == key {
            tail = prevKey
        }

        entry.prev = nil
        entry.next = head
        if let headKey = head, var currentHead = entries[headKey] {
            currentHead.prev = key
            entries[headKey] = currentHead
        }
        head = key
        entries[key] = entry
    }

    private func remove(_ key: UInt64) {
        guard let entry = entries[key] else { return }
        let prevKey = entry.prev
        let nextKey = entry.next

        if let prevKey, var prev = entries[prevKey] {
            prev.next = nextKey
            entries[prevKey] = prev
        } else {
            head = nextKey
        }
        if let nextKey, var next = entries[nextKey] {
            next.prev = prevKey
            entries[nextKey] = next
        } else {
            tail = prevKey
        }
        entries.removeValue(forKey: key)
    }
}

enum EmbeddingKey {
    static func make(text: String, identity: EmbeddingIdentity?, dimensions: Int, normalized: Bool) -> UInt64 {
        var hasher = FNV1a64()
        if let identity {
            hasher.append(identity.provider ?? "")
            hasher.append(identity.model ?? "")
            hasher.append(String(identity.dimensions ?? dimensions))
            hasher.append(String(identity.normalized ?? normalized))
        } else {
            hasher.append("nil_identity")
            hasher.append(String(dimensions))
            hasher.append(String(normalized))
        }
        hasher.append(text)
        return hasher.finalize()
    }
}

struct FNV1a64 {
    private var state: UInt64 = 14695981039346656037

    mutating func append(_ string: String) {
        for byte in string.utf8 {
            state ^= UInt64(byte)
            state &*= 1099511628211
        }
        state ^= 0xFF
        state &*= 1099511628211
    }

    mutating func finalize() -> UInt64 { state }
}
