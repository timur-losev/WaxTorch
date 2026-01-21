import Testing
import Wax

@Test func factualQueryClassification() {
    #expect(RuleBasedQueryClassifier.classify("What is the user's email address?") == .factual)
}

@Test func semanticQueryClassification() {
    #expect(RuleBasedQueryClassifier.classify("How does authentication relate to user privacy?") == .semantic)
}

@Test func temporalQueryClassification() {
    #expect(RuleBasedQueryClassifier.classify("What was discussed in yesterday's meeting?") == .temporal)
}

@Test func exploratoryQueryClassification() {
    #expect(RuleBasedQueryClassifier.classify("Tell me about the project") == .exploratory)
}

