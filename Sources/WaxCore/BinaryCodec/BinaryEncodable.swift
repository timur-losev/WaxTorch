import Foundation

public protocol BinaryEncodable {
    mutating func encode(to encoder: inout BinaryEncoder) throws
}

public protocol BinaryDecodable {
    static func decode(from decoder: inout BinaryDecoder) throws -> Self
}

public typealias BinaryCodable = BinaryEncodable & BinaryDecodable
