import 'dart:async';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:sqflite/sqflite.dart';

import 'chat_models.dart';

class ChatRepository {
  ChatRepository._();
  static final ChatRepository instance = ChatRepository._();

  Database? _db;
  final _conversationsController = StreamController<void>.broadcast();

  Stream<void> get conversationsChanged => _conversationsController.stream;

  Future<void> init() async {
    if (_db != null) return;
    final dir = await getApplicationDocumentsDirectory();
    final dbPath = p.join(dir.path, 'riftlink_chat.db');
    _db = await openDatabase(
      dbPath,
      version: 1,
      onCreate: (db, _) async {
        await db.execute('''
          CREATE TABLE conversations (
            id TEXT PRIMARY KEY,
            kind TEXT NOT NULL,
            peer_ref TEXT NOT NULL,
            title TEXT NOT NULL,
            subtitle TEXT,
            last_preview TEXT,
            last_at_ms INTEGER,
            unread_count INTEGER NOT NULL DEFAULT 0,
            archived INTEGER NOT NULL DEFAULT 0,
            pinned INTEGER NOT NULL DEFAULT 0,
            muted INTEGER NOT NULL DEFAULT 0,
            folder_id TEXT NOT NULL DEFAULT 'all'
          )
        ''');
        await db.execute('''
          CREATE TABLE messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id TEXT NOT NULL,
            from_id TEXT NOT NULL,
            to_id TEXT,
            group_id INTEGER,
            text TEXT NOT NULL,
            type TEXT NOT NULL,
            lane TEXT NOT NULL,
            direction TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at_ms INTEGER NOT NULL,
            msg_id INTEGER,
            rssi INTEGER,
            delivered INTEGER,
            total INTEGER,
            delete_at_ms INTEGER,
            relay_peers_json TEXT,
            relay_count INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
          )
        ''');
        await db.execute('''
          CREATE TABLE drafts (
            conversation_id TEXT PRIMARY KEY,
            text TEXT NOT NULL DEFAULT ''
          )
        ''');
        await db.execute('''
          CREATE TABLE folders (
            id TEXT PRIMARY KEY,
            title TEXT NOT NULL
          )
        ''');
        await db.insert('folders', {'id': 'all', 'title': 'All'});
        await db.insert('folders', {'id': 'personal', 'title': 'Personal'});
        await db.insert('folders', {'id': 'groups', 'title': 'Groups'});
        await db.insert('folders', {'id': 'archived', 'title': 'Archived'});

        await db.execute('CREATE INDEX idx_messages_conv_time ON messages(conversation_id, created_at_ms)');
        await db.execute('CREATE INDEX idx_messages_msgid ON messages(msg_id)');
        await db.execute('CREATE INDEX idx_conversations_unread ON conversations(unread_count)');
        await db.execute('CREATE INDEX idx_conversations_sort ON conversations(pinned, last_at_ms)');
      },
    );
  }

  Future<void> close() async {
    await _db?.close();
    _db = null;
    await _conversationsController.close();
  }

  Future<Database> get _database async {
    await init();
    return _db!;
  }

  static String directConversationId(String peerId) => 'direct:$peerId';
  static String groupConversationId(int groupId) => 'group:$groupId';
  static String broadcastConversationId() => 'broadcast:all';

  Future<void> ensureConversation({
    required String id,
    required ConversationKind kind,
    required String peerRef,
    required String title,
    String? subtitle,
  }) async {
    final db = await _database;
    await db.insert(
      'conversations',
      {
        'id': id,
        'kind': kind.name,
        'peer_ref': peerRef,
        'title': title,
        'subtitle': subtitle,
      },
      conflictAlgorithm: ConflictAlgorithm.ignore,
    );
  }

  Future<void> upsertConversationMeta(
    String id, {
    String? title,
    String? subtitle,
    String? lastPreview,
    int? lastAtMs,
    int? unreadCount,
    bool? archived,
    bool? pinned,
    bool? muted,
    String? folderId,
  }) async {
    final db = await _database;
    final update = <String, Object?>{};
    if (title != null) update['title'] = title;
    if (subtitle != null) update['subtitle'] = subtitle;
    if (lastPreview != null) update['last_preview'] = lastPreview;
    if (lastAtMs != null) update['last_at_ms'] = lastAtMs;
    if (unreadCount != null) update['unread_count'] = unreadCount;
    if (archived != null) update['archived'] = archived ? 1 : 0;
    if (pinned != null) update['pinned'] = pinned ? 1 : 0;
    if (muted != null) update['muted'] = muted ? 1 : 0;
    if (folderId != null) update['folder_id'] = folderId;
    if (update.isEmpty) return;
    await db.update('conversations', update, where: 'id = ?', whereArgs: [id]);
    _conversationsController.add(null);
  }

  Future<int> appendMessage(ChatMessage message, {bool incrementUnread = false}) async {
    final db = await _database;
    final nowMs = DateTime.now().millisecondsSinceEpoch;
    final insertedId = await db.insert(
      'messages',
      {
        'conversation_id': message.conversationId,
        'from_id': message.from,
        'to_id': message.to,
        'group_id': message.groupId,
        'text': message.text,
        'type': message.type,
        'lane': message.lane,
        'direction': message.direction.name,
        'status': message.status.name,
        'created_at_ms': message.createdAtMs,
        'msg_id': message.msgId,
        'rssi': message.rssi,
        'delivered': message.delivered,
        'total': message.total,
        'delete_at_ms': message.deleteAtMs,
        'relay_peers_json': relayPeersToJson(message.relayPeers),
        'relay_count': message.relayCount,
      },
    );

    final conv = await db.query(
      'conversations',
      columns: ['unread_count'],
      where: 'id = ?',
      whereArgs: [message.conversationId],
      limit: 1,
    );
    final currentUnread = conv.isNotEmpty ? ((conv.first['unread_count'] as int?) ?? 0) : 0;
    await db.update(
      'conversations',
      {
        'last_preview': _buildPreview(message),
        'last_at_ms': message.createdAtMs,
        'unread_count': incrementUnread ? currentUnread + 1 : currentUnread,
      },
      where: 'id = ?',
      whereArgs: [message.conversationId],
    );

    // Auto-fix stale "no timestamp" entries.
    await db.rawUpdate(
      'UPDATE messages SET created_at_ms = ? WHERE conversation_id = ? AND created_at_ms <= 0',
      [nowMs, message.conversationId],
    );
    _conversationsController.add(null);
    return insertedId;
  }

  String _buildPreview(ChatMessage message) {
    if (message.type == 'voice') return 'Voice message';
    if (message.type == 'location') return 'Location';
    return message.text;
  }

  Future<List<ChatConversation>> listConversations({
    bool includeArchived = false,
    String folderId = 'all',
    String query = '',
  }) async {
    final db = await _database;
    final where = <String>[];
    final args = <Object?>[];
    if (!includeArchived) {
      where.add('archived = 0');
    }
    if (folderId != 'all') {
      where.add('folder_id = ?');
      args.add(folderId);
    }
    if (query.trim().isNotEmpty) {
      where.add('(title LIKE ? OR last_preview LIKE ?)');
      final q = '%${query.trim()}%';
      args.add(q);
      args.add(q);
    }

    final rows = await db.query(
      'conversations',
      where: where.isEmpty ? null : where.join(' AND '),
      whereArgs: args,
      orderBy: 'pinned DESC, last_at_ms DESC',
    );
    return rows.map(_conversationFromRow).toList();
  }

  Future<List<ChatConversation>> listArchivedConversations() async {
    final db = await _database;
    final rows = await db.query(
      'conversations',
      where: 'archived = 1',
      orderBy: 'pinned DESC, last_at_ms DESC',
    );
    return rows.map(_conversationFromRow).toList();
  }

  Future<ChatConversation?> getConversation(String conversationId) async {
    final db = await _database;
    final rows = await db.query('conversations', where: 'id = ?', whereArgs: [conversationId], limit: 1);
    if (rows.isEmpty) return null;
    return _conversationFromRow(rows.first);
  }

  Future<List<ChatMessage>> listMessages(String conversationId, {int limit = 300}) async {
    final db = await _database;
    final rows = await db.query(
      'messages',
      where: 'conversation_id = ?',
      whereArgs: [conversationId],
      orderBy: 'created_at_ms ASC',
      limit: limit,
    );
    return rows.map(_messageFromRow).toList();
  }

  Future<void> markConversationRead(String conversationId) async {
    final db = await _database;
    await db.update(
      'conversations',
      {'unread_count': 0},
      where: 'id = ?',
      whereArgs: [conversationId],
    );
    _conversationsController.add(null);
  }

  Future<void> setPinned(String conversationId, bool value) async {
    await upsertConversationMeta(conversationId, pinned: value);
  }

  Future<void> setArchived(String conversationId, bool value) async {
    await upsertConversationMeta(
      conversationId,
      archived: value,
      folderId: value ? 'archived' : 'all',
    );
  }

  Future<void> setMuted(String conversationId, bool value) async {
    await upsertConversationMeta(conversationId, muted: value);
  }

  Future<void> setFolder(String conversationId, String folderId) async {
    await upsertConversationMeta(conversationId, folderId: folderId);
  }

  Future<void> deleteConversation(String conversationId) async {
    final db = await _database;
    await db.delete('messages', where: 'conversation_id = ?', whereArgs: [conversationId]);
    await db.delete('drafts', where: 'conversation_id = ?', whereArgs: [conversationId]);
    await db.delete('conversations', where: 'id = ?', whereArgs: [conversationId]);
    _conversationsController.add(null);
  }

  Future<void> clearAll() async {
    final db = await _database;
    await db.delete('messages');
    await db.delete('drafts');
    await db.delete('conversations');
    _conversationsController.add(null);
  }

  Future<void> setDraft(String conversationId, String text) async {
    final db = await _database;
    await db.insert(
      'drafts',
      {
        'conversation_id': conversationId,
        'text': text,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
  }

  Future<String> getDraft(String conversationId) async {
    final db = await _database;
    final rows = await db.query('drafts', where: 'conversation_id = ?', whereArgs: [conversationId], limit: 1);
    if (rows.isEmpty) return '';
    return (rows.first['text'] as String?) ?? '';
  }

  Future<void> updateByMsgId({
    required int msgId,
    required String conversationId,
    MessageStatus? status,
    int? delivered,
    int? total,
  }) async {
    final db = await _database;
    final payload = <String, Object?>{};
    if (status != null) payload['status'] = status.name;
    if (delivered != null) payload['delivered'] = delivered;
    if (total != null) payload['total'] = total;
    if (payload.isEmpty) return;
    await db.update(
      'messages',
      payload,
      where: 'conversation_id = ? AND msg_id = ?',
      whereArgs: [conversationId, msgId],
    );
  }

  ChatConversation _conversationFromRow(Map<String, Object?> row) {
    return ChatConversation(
      id: row['id'] as String,
      kind: ConversationKind.values.firstWhere(
        (v) => v.name == row['kind'],
        orElse: () => ConversationKind.direct,
      ),
      peerRef: row['peer_ref'] as String,
      title: (row['title'] as String?) ?? '',
      subtitle: row['subtitle'] as String?,
      lastMessagePreview: row['last_preview'] as String?,
      lastMessageAtMs: row['last_at_ms'] as int?,
      unreadCount: (row['unread_count'] as int?) ?? 0,
      archived: ((row['archived'] as int?) ?? 0) == 1,
      pinned: ((row['pinned'] as int?) ?? 0) == 1,
      muted: ((row['muted'] as int?) ?? 0) == 1,
      folderId: (row['folder_id'] as String?) ?? 'all',
    );
  }

  ChatMessage _messageFromRow(Map<String, Object?> row) {
    return ChatMessage(
      id: row['id'] as int?,
      conversationId: row['conversation_id'] as String,
      from: row['from_id'] as String? ?? '',
      to: row['to_id'] as String?,
      groupId: row['group_id'] as int?,
      text: row['text'] as String? ?? '',
      type: row['type'] as String? ?? 'text',
      lane: row['lane'] as String? ?? 'normal',
      direction: MessageDirection.values.firstWhere(
        (v) => v.name == row['direction'],
        orElse: () => MessageDirection.incoming,
      ),
      status: MessageStatus.values.firstWhere(
        (v) => v.name == row['status'],
        orElse: () => MessageStatus.pending,
      ),
      createdAtMs: row['created_at_ms'] as int? ?? 0,
      msgId: row['msg_id'] as int?,
      rssi: row['rssi'] as int?,
      delivered: row['delivered'] as int?,
      total: row['total'] as int?,
      deleteAtMs: row['delete_at_ms'] as int?,
      relayPeers: relayPeersFromJson(row['relay_peers_json'] as String?),
      relayCount: (row['relay_count'] as int?) ?? 0,
    );
  }
}

