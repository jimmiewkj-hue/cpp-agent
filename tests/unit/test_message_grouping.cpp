// Test MessageGrouping ? aligned with local-ace compact/grouping.ts
#include "compact/MessageGrouping.h"
#include "core/AgentTypes.h"

#include <cassert>
#include <iostream>

static int failures = 0;
static void Check(bool c, const char* l) { if(!c){std::cerr<<"FAIL: "<<l<<std::endl;++failures;} }

using namespace agent::core;

void TestEmptyMessages() {
  auto groups = agent::compact::GroupMessagesByApiRound({});
  Check(groups.empty(), "Empty messages -> empty groups");
}

void TestSingleUserMessage() {
  std::vector<Message> msgs;
  Message m; m.role = MessageRole::User; m.uuid = "u1";
  m.content.push_back(ContentBlock::MakeText("hello"));
  msgs.push_back(m);
  auto groups = agent::compact::GroupMessagesByApiRound(msgs);
  Check(groups.size() == 1, "Single user -> 1 group");
  Check(groups[0].size() == 1, "Single user group has 1 message");
}

void TestSingleAssistantRound() {
  std::vector<Message> msgs;
  Message u; u.role = MessageRole::User; u.uuid = "u1";
  u.content.push_back(ContentBlock::MakeText("task"));
  msgs.push_back(u);
  
  Message a; a.role = MessageRole::Assistant; a.uuid = "a1";
  a.content.push_back(ContentBlock::MakeText("response"));
  msgs.push_back(a);
  
  Message tr; tr.role = MessageRole::User; tr.uuid = "tr1"; tr.isMeta = true;
  tr.content.push_back(ContentBlock::MakeToolResult("tid1", "result", false));
  msgs.push_back(tr);
  
  auto groups = agent::compact::GroupMessagesByApiRound(msgs);
  // API-round grouping: initial user prompt is group 0, assistant+tool_result is group 1
  Check(groups.size() == 2, "Single round -> 2 groups (user prompt + assistant round)");
  Check(groups[0].size() == 1, "Group 0 has user message only");
  Check(groups[1].size() == 2, "Group 1 has assistant + tool result");
}

void TestMultipleRounds() {
  std::vector<Message> msgs;
  
  // Round 1
  Message u1; u1.role = MessageRole::User; u1.uuid = "u1";
  u1.content.push_back(ContentBlock::MakeText("round1"));
  msgs.push_back(u1);
  
  Message a1; a1.role = MessageRole::Assistant; a1.uuid = "a1";
  a1.content.push_back(ContentBlock::MakeText("response1"));
  msgs.push_back(a1);
  
  Message tr1; tr1.role = MessageRole::User; tr1.uuid = "tr1"; tr1.isMeta = true;
  tr1.content.push_back(ContentBlock::MakeToolResult("t1", "ok1", false));
  msgs.push_back(tr1);
  
  // Round 2 ? new assistant uuid
  Message a2; a2.role = MessageRole::Assistant; a2.uuid = "a2";
  a2.content.push_back(ContentBlock::MakeText("response2"));
  msgs.push_back(a2);
  
  Message tr2; tr2.role = MessageRole::User; tr2.uuid = "tr2"; tr2.isMeta = true;
  tr2.content.push_back(ContentBlock::MakeToolResult("t2", "ok2", false));
  msgs.push_back(tr2);
  
  auto groups = agent::compact::GroupMessagesByApiRound(msgs);
  // Groups: [user-u1], [assistant-a1 + tool_result-tr1], [assistant-a2 + tool_result-tr2]
  Check(groups.size() == 3, "Two rounds -> 3 groups (user + round1 + round2)");
  Check(groups[0].size() == 1, "Group 0 has user prompt");
  Check(groups[1].size() == 2, "Group 1 has round 1 (assistant + tool result)");
  Check(groups[2].size() == 2, "Group 2 has round 2 (assistant + tool result)");
}

void TestSameAssistantIdMultipleBlocks() {
  std::vector<Message> msgs;
  
  // Two assistant messages with same uuid (streaming chunks)
  Message a1; a1.role = MessageRole::Assistant; a1.uuid = "a1";
  a1.content.push_back(ContentBlock::MakeText("part1"));
  msgs.push_back(a1);
  
  Message tr; tr.role = MessageRole::User; tr.uuid = "tr1"; tr.isMeta = true;
  tr.content.push_back(ContentBlock::MakeToolResult("t1", "ok", false));
  msgs.push_back(tr);
  
  Message a1b; a1b.role = MessageRole::Assistant; a1b.uuid = "a1"; // SAME id
  a1b.content.push_back(ContentBlock::MakeText("part2"));
  msgs.push_back(a1b);
  
  auto groups = agent::compact::GroupMessagesByApiRound(msgs);
  Check(groups.size() == 1, "Same assistant id -> 1 group");
  Check(groups[0].size() == 3, "All chunks in one group");
}

int main() {
  std::cout << "=== MessageGrouping Tests ===" << std::endl;
  TestEmptyMessages();
  TestSingleUserMessage();
  TestSingleAssistantRound();
  TestMultipleRounds();
  TestSameAssistantIdMultipleBlocks();
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
