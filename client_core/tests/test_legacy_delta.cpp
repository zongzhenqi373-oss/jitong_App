#include <iostream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "client_core/storage/NativeDatabase.h"

using namespace im::storage;
namespace { int fails=0; void ck(bool v,const char*n){std::cout<<(v?"[PASS] ":"[FAIL] ")<<n<<"\n";if(!v)++fails;}
long long qi(NativeDatabase& db,const std::string& sql){long long v=-1;db.withRead([&](sqlite3*d){sqlite3_stmt*s=nullptr;if(sqlite3_prepare_v2(d,sql.c_str(),-1,&s,nullptr)==SQLITE_OK&&sqlite3_step(s)==SQLITE_ROW)v=sqlite3_column_int64(s,0);if(s)sqlite3_finalize(s);});return v;}
std::string qt(NativeDatabase& db,const std::string& sql){std::string v;db.withRead([&](sqlite3*d){sqlite3_stmt*s=nullptr;if(sqlite3_prepare_v2(d,sql.c_str(),-1,&s,nullptr)==SQLITE_OK&&sqlite3_step(s)==SQLITE_ROW){auto*p=sqlite3_column_text(s,0);if(p)v=(const char*)p;}if(s)sqlite3_finalize(s);});return v;}
LegacyDeltaChange msg(long seq,std::string op,std::string content="hello"){LegacyDeltaChange c;c.changeSeq=seq;c.ownerId=1;c.entityType="MESSAGE";c.entityKey="m1";c.operation=op;c.changedAt=100+seq;if(op=="UPSERT"){c.payloadVersion=1;c.payload="{\"msgId\":\"m1\",\"conversationId\":10,\"peerId\":2,\"seq\":1,\"ts\":100,\"localOrder\":1,\"fromMe\":1,\"type\":0,\"content\":\""+content+"\",\"status\":1,\"pinyin\":\"hello\",\"initials\":\"h\",\"mediaPath\":\"\",\"imgW\":0,\"imgH\":0,\"fileId\":\"\",\"fileName\":\"\",\"fileSize\":0,\"contentType\":\"\",\"sha256\":\"\",\"thumbnailFileId\":\"\",\"thumbnailPath\":\"\",\"thumbnailSize\":0,\"thumbnailSha256\":\"\",\"thumbnailW\":0,\"thumbnailH\":0,\"largeThumbnailFileId\":\"\",\"largeThumbnailPath\":\"\",\"largeThumbnailSize\":0,\"largeThumbnailSha256\":\"\",\"largeThumbnailW\":0,\"largeThumbnailH\":0,\"localPath\":\"\",\"transferred\":0}";}return c;}
LegacyDeltaChange conv(long seq,std::string op){LegacyDeltaChange c;c.changeSeq=seq;c.ownerId=1;c.entityType="CONVERSATION";c.entityKey="10";c.operation=op;c.changedAt=100+seq;if(op=="UPSERT"){c.payloadVersion=1;c.payload="{\"conversationId\":10,\"peerId\":2,\"lastMsg\":\"hello\",\"lastTs\":100,\"unread\":3}";}return c;}
}
int main(){std::system("rm -rf /tmp/test_legacy_delta && mkdir -p /tmp/test_legacy_delta");NativeDatabase db;std::string e;ck(db.open("/tmp/test_legacy_delta",1,std::vector<unsigned char>(32,7),&e),"open");ck(db.beginMigration(1,&e),"begin shadow import");
 ck(db.seedLegacyDeltaCheckpoint(1,2,50,100,&e),"seed snapshot baseline");
 ck(db.seedLegacyDeltaCheckpoint(1,2,50,101,&e),"same baseline replay idempotent");
 ck(!db.seedLegacyDeltaCheckpoint(1,2,51,102,&e),"different baseline rejected");
 ck(qi(db,"SELECT checkpoint_value FROM migration_checkpoint WHERE epoch=2 AND stream='legacy_delta'")==50,"baseline persisted");
 auto r=db.submitLegacyDeltaBatch(1,1,0,{msg(2,"UPSERT"),conv(4,"UPSERT")});ck(r.ok&&r.committedCheckpoint==4,"upsert+checkpoint");ck(qi(db,"SELECT count(*) FROM messages WHERE msg_id='m1'")==1,"message inserted");ck(qt(db,"SELECT content FROM messages WHERE msg_id='m1'")=="hello","content");ck(qi(db,"SELECT count(*) FROM message_fts_identity WHERE msg_id='m1'")==1,"fts");ck(qi(db,"SELECT unread FROM conversations WHERE conversation_id=10")==3,"conversation");
 auto u=db.submitLegacyDeltaBatch(1,1,4,{msg(7,"UPSERT","updated")});ck(u.ok&&qt(db,"SELECT content FROM messages WHERE msg_id='m1'")=="updated","update");ck(qi(db,"SELECT count(*) FROM message_fts_identity WHERE msg_id='m1'")==1,"update replaces fts once");
 auto malformed=msg(8,"UPSERT");malformed.payload="{";auto mf=db.submitLegacyDeltaBatch(1,1,7,{malformed});ck(!mf.ok,"malformed payload rejected");ck(qi(db,"SELECT checkpoint_value FROM migration_checkpoint WHERE epoch=1 AND stream='legacy_delta'")==7,"malformed payload rolls checkpoint back");
 auto wrongOwner=msg(8,"DELETE");wrongOwner.ownerId=2;ck(!db.submitLegacyDeltaBatch(1,1,7,{wrongOwner}).ok,"mixed owner rejected");
 auto bad=db.submitLegacyDeltaBatch(1,1,7,{msg(9,"UPSERT"),msg(8,"DELETE")});ck(!bad.ok,"reject unordered");ck(qi(db,"SELECT checkpoint_value FROM migration_checkpoint WHERE epoch=1 AND stream='legacy_delta'")==7,"checkpoint rollback");ck(qt(db,"SELECT content FROM messages WHERE msg_id='m1'")=="updated","data rollback");
 auto del=db.submitLegacyDeltaBatch(1,1,7,{msg(10,"DELETE"),conv(11,"DELETE")});ck(del.ok&&del.committedCheckpoint==11,"delete");ck(qi(db,"SELECT count(*) FROM messages WHERE msg_id='m1'")==0,"message deleted");ck(qi(db,"SELECT count(*) FROM message_fts_identity WHERE msg_id='m1'")==0,"fts deleted");ck(qi(db,"SELECT count(*) FROM conversations WHERE conversation_id=10")==0,"conversation deleted");
 auto replay=db.submitLegacyDeltaBatch(1,1,7,{msg(10,"DELETE"),conv(11,"DELETE")});ck(replay.ok&&replay.committedCheckpoint==11,"whole committed batch replay is idempotent");
 auto overlap=db.submitLegacyDeltaBatch(1,1,10,{conv(11,"DELETE"),msg(12,"DELETE")});ck(!overlap.ok,"batch crossing checkpoint rejected");
 CutoverJournalSnapshot journal;ck(db.queryCutoverJournal(1,&journal)&&!journal.present,"journal initially missing");
 ck(db.advanceCutoverJournal(1,1,-1,CutoverJournalState::LegacyActive,11,10,"kid","sum",1000,&e),"create legacy journal");
 ck(db.advanceCutoverJournal(1,1,0,CutoverJournalState::Prepared,11,10,"kid","sum",1001,&e),"legacy to prepared");
 ck(!db.advanceCutoverJournal(1,1,1,CutoverJournalState::NativeCommittedDirty,11,10,"kid","sum",1002,&e),"cannot skip no-write");
 ck(!db.advanceCutoverJournal(1,2,1,CutoverJournalState::NativeCommittedNoWrite,11,10,"kid","sum",1002,&e),"cannot change epoch");
 ck(db.advanceCutoverJournal(1,1,1,CutoverJournalState::NativeCommittedNoWrite,11,10,"kid","sum",1002,&e),"prepared to no-write");
 int dirtyNotifications=0;CutoverJournalSnapshot notified;
 db.setCutoverDirtyListener([&](const CutoverJournalSnapshot& s){++dirtyNotifications;notified=s;});
 bool timed=false;ck(db.submitBusinessSync([](sqlite3*){return false;},std::chrono::seconds(1),&timed)==CommandResult::SqlFailed,"failed business tx rolls back");
 ck(db.queryCutoverJournal(1,&journal)&&journal.state==CutoverJournalState::NativeCommittedNoWrite,"failed business tx keeps no-write");
 ck(db.submitBusinessSync([](sqlite3*d){return sqlite3_exec(d,"INSERT INTO friends(owner_id,friend_id,nick) VALUES(1,99,'x')",nullptr,nullptr,nullptr)==SQLITE_OK;},std::chrono::seconds(1),&timed)==CommandResult::Ok,"first business tx commits");
 ck(db.queryCutoverJournal(1,&journal)&&journal.state==CutoverJournalState::NativeCommittedDirty,"business hook atomically marks dirty");
 ck(dirtyNotifications==1&&notified.present&&notified.epoch==1&&
    notified.state==CutoverJournalState::NativeCommittedDirty&&notified.highWater==11&&
    notified.schemaVersion==10&&notified.keyId=="kid"&&notified.summary=="sum"&&
    notified.updatedAt==journal.updatedAt,"dirty listener fired once with committed snapshot");
 // 已是 DIRTY 的后续业务写不再重复推进/通知
 ck(db.submitBusinessSync([](sqlite3*d){return sqlite3_exec(d,"UPDATE friends SET nick='y' WHERE friend_id=99",nullptr,nullptr,nullptr)==SQLITE_OK;},std::chrono::seconds(1),&timed)==CommandResult::Ok,"second business tx commits");
 ck(dirtyNotifications==1,"dirty listener not re-fired when already dirty");
 ck(!db.advanceCutoverJournal(1,1,3,CutoverJournalState::NativeCommittedNoWrite,11,10,"kid","sum",1004,&e),"dirty cannot roll back");
 ck(db.queryCutoverJournal(1,&journal)&&journal.present&&journal.epoch==1&&
    journal.state==CutoverJournalState::NativeCommittedDirty&&journal.highWater==11,"query dirty journal");
 db.close();std::system("rm -rf /tmp/test_legacy_delta");return fails?1:0;}
