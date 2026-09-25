#include "inventory/kafka_io.hpp"

#include <stdexcept>

#include <librdkafka/rdkafkacpp.h>

#include "inventory/obs.hpp"

namespace inventory::kafka {

namespace {

std::unique_ptr<RdKafka::Conf> make_conf(const std::string& brokers) {
  std::unique_ptr<RdKafka::Conf> conf(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  std::string err;
  if (conf->set("bootstrap.servers", brokers, err) != RdKafka::Conf::CONF_OK) {
    throw std::runtime_error("kafka conf bootstrap.servers: " + err);
  }
  return conf;
}

}  // namespace

Producer::Producer(const std::string& brokers) {
  auto conf = make_conf(brokers);
  std::string err;
  conf->set("enable.idempotence", "true", err);
  producer_.reset(RdKafka::Producer::create(conf.get(), err));
  if (!producer_) {
    throw std::runtime_error("kafka producer create: " + err);
  }
}

Producer::~Producer() {
  if (producer_) {
    producer_->flush(5000);
  }
}

void Producer::publish(const std::string& topic, const std::string& key,
                       const std::string& payload, const std::string& traceparent) {
  RdKafka::Headers* headers = nullptr;
  if (!traceparent.empty()) {
    headers = RdKafka::Headers::create();
    headers->add(kTraceparentHeader, traceparent);
  }

  const RdKafka::ErrorCode ec = producer_->produce(
      topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
      const_cast<char*>(payload.data()), payload.size(), key.data(), key.size(),
      /*timestamp=*/0, headers, /*msg_opaque=*/nullptr);
  if (ec != RdKafka::ERR_NO_ERROR) {
    delete headers;  // ownership only transfers on success
    throw std::runtime_error("kafka produce: " + RdKafka::err2str(ec));
  }
  // Synchronous flush keeps the outbox relay's "publish then mark" simple.
  producer_->flush(5000);
  if (producer_->outq_len() > 0) {
    throw std::runtime_error("kafka publish not fully flushed");
  }
}

Consumer::Consumer(const std::string& brokers, const std::string& group,
                   const std::vector<std::string>& topics) {
  auto conf = make_conf(brokers);
  std::string err;
  conf->set("group.id", group, err);
  conf->set("enable.auto.commit", "false", err);
  conf->set("auto.offset.reset", "earliest", err);
  consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), err));
  if (!consumer_) {
    throw std::runtime_error("kafka consumer create: " + err);
  }
  std::vector<std::string> mutable_topics = topics;
  const RdKafka::ErrorCode ec = consumer_->subscribe(mutable_topics);
  if (ec != RdKafka::ERR_NO_ERROR) {
    throw std::runtime_error("kafka subscribe: " + RdKafka::err2str(ec));
  }
}

Consumer::~Consumer() {
  if (consumer_) {
    consumer_->close();
  }
}

void Consumer::run(const Handler& handler) {
  running_ = true;
  while (running_) {
    std::unique_ptr<RdKafka::Message> msg(consumer_->consume(1000));
    switch (msg->err()) {
      case RdKafka::ERR__TIMED_OUT:
      case RdKafka::ERR__PARTITION_EOF:
        continue;
      case RdKafka::ERR_NO_ERROR:
        break;
      default:
        obs::log_error("kafka consume error", {{"error", msg->errstr()}});
        continue;
    }

    std::string traceparent;
    if (const RdKafka::Headers* headers = msg->headers()) {
      for (const auto& h : headers->get(kTraceparentHeader)) {
        traceparent.assign(static_cast<const char*>(h.value()), h.value_size());
      }
    }
    const std::string value(static_cast<const char*>(msg->payload()), msg->len());

    try {
      handler(msg->topic_name(), value, traceparent);
    } catch (const std::exception& ex) {
      // Do not commit; the message is redelivered and idempotency dedupes it.
      obs::log_error("handler failed; message will be redelivered",
                     {{"error", ex.what()}});
      continue;
    }
    consumer_->commitSync(msg.get());
  }
}

void Consumer::stop() { running_ = false; }

}  // namespace inventory::kafka
